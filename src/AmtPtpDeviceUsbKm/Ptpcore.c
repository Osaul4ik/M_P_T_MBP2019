// PTPCore.c - Frame orchestration: Phase A (lift) -> Phase B (birth) -> Phase C (update).

#include "Driver.h"
#include "PTPCore.h"
#include "PTPCore.tmh"
#include "ActiveContact.h"
#include "Match.h"
#include "Gesture.h"

// Movement past this distance (normalized units - same coordinate space
// as RETAP_MAX_DISTANCE in ActiveContact.h) from the button-down anchor
// latches ForceTouchDragLockout for the rest of the press: once the user
// is visibly dragging (e.g. moving a window) after a hard tap, a deeper
// press must not fire a synthetic right-click on top of that drag.
#define FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE 60

// How long PTPCore will wait, once the button goes down, for the press
// to either cross FORCE_TOUCH_PRESSURE_THRESHOLD or start receding
// before giving up and committing it as an ordinary click. "Some time"
// per the design note - tunable.
#define CLICK_ARBITRATION_TIMEOUT_MS 80

// A press is considered to be receding once its pressure falls this many
// raw units below the highest value seen so far this press (rather than
// below the immediately preceding frame - the sensor is noisy enough
// that a single frame-to-frame dip isn't reliable on its own). Tunable;
// ~3-5 units is enough margin to absorb sensor noise without meaningfully
// delaying the decision.
#define CLICK_ARBITRATION_PRESSURE_HYSTERESIS 4

// Recent-lift ring buffer (slot-independent retap memory)

VOID
AmtRecentLiftRecord(
    _Inout_ RECENT_LIFT_RING* Ring,
    _In_    LONGLONG          NowQpc,
    _In_    USHORT            X,
    _In_    USHORT            Y
)
{
    UCHAR idx = Ring->NextWriteIndex;
    Ring->Entries[idx].Valid   = TRUE;
    Ring->Entries[idx].LiftQpc = NowQpc;
    Ring->Entries[idx].X       = X;
    Ring->Entries[idx].Y       = Y;
    Ring->NextWriteIndex = (UCHAR)((idx + 1) % RECENT_LIFT_CAPACITY);
}

BOOLEAN
AmtRecentLiftFindNearby(
    _In_  const RECENT_LIFT_RING* Ring,
    _In_  LONGLONG                NowQpc,
    _In_  LONGLONG                PerfFrequencyHz,
    _In_  USHORT                  CandX,
    _In_  USHORT                  CandY,
    _Out_ USHORT*                 OutX,
    _Out_ USHORT*                 OutY
)
{
    if (PerfFrequencyHz <= 0)
        return FALSE;

    LONGLONG windowTicks = (RETAP_WINDOW_100NS * PerfFrequencyHz) / 10000000LL;

    LONG     bestDistSq = -1;
    BOOLEAN  found       = FALSE;
    USHORT   bestX = 0, bestY = 0;

    for (UCHAR i = 0; i < RECENT_LIFT_CAPACITY; i++) {
        const RECENT_LIFT* e = &Ring->Entries[i];
        if (!e->Valid)
            continue;
        if (NowQpc < e->LiftQpc)
            continue; // QPC must be monotonic
        if (NowQpc - e->LiftQpc > windowTicks)
            continue;

        INT dx = (INT)CandX - (INT)e->X;
        INT dy = (INT)CandY - (INT)e->Y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;

        if (dx > RETAP_MAX_DISTANCE || dy > RETAP_MAX_DISTANCE)
            continue;

        LONG distSq = (LONG)dx * dx + (LONG)dy * dy;
        if (!found || distSq < bestDistSq) {
            bestDistSq = distSq;
            bestX      = e->X;
            bestY      = e->Y;
            found      = TRUE;
        }
    }

    if (found) {
        *OutX = bestX;
        *OutY = bestY;
    }
    return found;
}

// Emit a contact report of any phase; overflow-queue if the frame is full.
// AUDIT FIX: previously this only ever emitted CONTACT_PHASE_UP (as
// AmtCoreEmitLift) and Phase C's DOWN/MOVE reports had NO overflow
// fallback at all - if ContactCount was already at PTP_MAX_CONTACT_POINTS
// (e.g. all 5 fingers down + a button click in the same frame, where
// Phase A.5 below fills every report slot with synthetic lift-offs),
// Phase C's brand-new-ContactID DOWN reports were silently dropped
// outright, leaving Windows with a ContactID it never saw go down.
static VOID
AmtCoreEmitContact(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _Inout_ PTP_CORE_FRAME* OutResult,
    _In_    ULONG           ContactID,
    _In_    USHORT          X,
    _In_    USHORT          Y,
    _In_    CONTACT_PHASE   Phase,
    _In_    BOOLEAN         Confident
)
{
    if (OutResult->ContactCount < PTP_MAX_CONTACT_POINTS) {
        PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
        outC->ContactID   = ContactID;
        outC->X           = X;
        outC->Y           = Y;
        outC->Phase       = Phase;
        outC->Confident   = Confident;
        outC->PalmSuspect = FALSE;
        OutResult->ContactCount++;
        return;
    }

    if (pCtx->OverflowCount < PTP_MAX_CONTACT_POINTS) {
        pCtx->OverflowContactID[pCtx->OverflowCount] = ContactID;
        pCtx->OverflowX[pCtx->OverflowCount]         = X;
        pCtx->OverflowY[pCtx->OverflowCount]         = Y;
        pCtx->OverflowPhase[pCtx->OverflowCount]     = Phase;
        pCtx->OverflowConfident[pCtx->OverflowCount] = Confident;
        pCtx->OverflowCount++;
    }
    // else: overflow queue full too - event is dropped this frame.
    // Would need > 2*PTP_MAX_CONTACT_POINTS events in flight
    // simultaneously; not reachable with PTP_MAX_CONTACT_POINTS physical
    // fingers (worst case is PTP_MAX_CONTACT_POINTS UPs + the same number
    // of DOWNs from an all-fingers-down button click, which exactly fits
    // one in-frame report plus one full overflow queue).
}

// Drain deferred reports from previous frame(s).
static VOID
AmtCoreDrainOverflow(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _Inout_ PTP_CORE_FRAME* OutResult
)
{
    UCHAR k = 0;
    for (; k < pCtx->OverflowCount && OutResult->ContactCount < PTP_MAX_CONTACT_POINTS; k++) {
        PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
        outC->ContactID   = pCtx->OverflowContactID[k];
        outC->X           = pCtx->OverflowX[k];
        outC->Y           = pCtx->OverflowY[k];
        outC->Phase       = pCtx->OverflowPhase[k];
        outC->Confident   = pCtx->OverflowConfident[k];
        outC->PalmSuspect = FALSE;
        OutResult->ContactCount++;
    }

    // AUDIT FIX: entries beyond what fit this frame used to be discarded
    // outright (OverflowCount was unconditionally zeroed below regardless
    // of k). Not reachable today - DrainOverflow runs against a freshly
    // zeroed OutResult before anything else touches it, and OverflowCount
    // is itself capped at PTP_MAX_CONTACT_POINTS, so k always reaches
    // OverflowCount before the capacity check can fail. Compacting the
    // (currently always-empty) tail down to index 0 instead of an
    // unconditional reset costs nothing and keeps this function correct
    // by construction rather than by coincidence of today's call order.
    UCHAR remaining = (UCHAR)(pCtx->OverflowCount - k);
    if (remaining > 0) {
        RtlMoveMemory(pCtx->OverflowContactID, &pCtx->OverflowContactID[k], remaining * sizeof(ULONG));
        RtlMoveMemory(pCtx->OverflowX,         &pCtx->OverflowX[k],         remaining * sizeof(USHORT));
        RtlMoveMemory(pCtx->OverflowY,         &pCtx->OverflowY[k],         remaining * sizeof(USHORT));
        RtlMoveMemory(pCtx->OverflowPhase,     &pCtx->OverflowPhase[k],     remaining * sizeof(CONTACT_PHASE));
        RtlMoveMemory(pCtx->OverflowConfident, &pCtx->OverflowConfident[k], remaining * sizeof(BOOLEAN));
    }
    pCtx->OverflowCount = remaining;
}

// PTPCore_ProcessFrame

VOID
PTPCore_ProcessFrame(
    _Inout_ PDEVICE_CONTEXT  DeviceContext,
    _In_    const RAW_FRAME* RawFrame,
    _In_    LONGLONG         NowQpc,
    _In_    BOOLEAN          ButtonDown,
    _Out_   PTP_CORE_FRAME*  OutResult,
    _Out_   BOOLEAN*         OutForceTouchDownEdge,
    _Out_   BOOLEAN*         OutForceTouchUpEdge,
    _Out_   BOOLEAN*         OutButtonClickReport
)
{
    PDEVICE_CONTEXT pCtx = DeviceContext;

    // Click-edge detection for the button-rebirth workaround (Phase A.5
    // below). Only the RISING edge (0->1) matters: that's the frame where
    // Windows' PTP integrated-button anti-jitter logic would otherwise
    // compare the live HID coordinate against the current cursor position
    // and "snap" the click to the cursor if the delta looks like jitter.
    // Forcing a real Kill->Birth of the live contact's ContactID at its
    // own current position routes this click through the ordinary
    // soft-tap TipSwitch path instead, which isn't subject to that snap.
    BOOLEAN buttonClickEdge = ButtonDown && !pCtx->PrevButtonClicked;
    pCtx->PrevButtonClicked = ButtonDown;

    RtlZeroMemory(OutResult, sizeof(PTP_CORE_FRAME));
    OutResult->TimestampQpc = NowQpc;

    // Build candidates (palm + tip-debounce)
    MATCH_CANDIDATE_SET candidates;
    BOOLEAN              largePalm = FALSE;

    AmtMatchBuildCandidates(RawFrame, pCtx->DeviceInfo, pCtx->ActiveContacts,
                            &candidates, &largePalm);

    // Palm session: suppress candidates when palm active. Contacts that
    // go unmatched purely because of this suppression are NOT lifted
    // (see Phase A below) - they're frozen at low confidence instead,
    // so no phantom UP/tap fires while the hand is still on the pad.
    BOOLEAN palmSuppressedFrame = FALSE;

    if (largePalm) {
        pCtx->PalmDetected = TRUE;
        OutResult->LargePalmBlanked = TRUE;
        palmSuppressedFrame = TRUE;
    } else if (pCtx->PalmDetected) {
        BOOLEAN anyContact = (candidates.Count > 0);
        if (!anyContact) {
            pCtx->PalmDetected = FALSE;
        } else {
            candidates.Count = 0; // still palm-adjacent - suppress all
            palmSuppressedFrame = TRUE;
        }
    }

    // Cost-based correspondence
    MATCH_RESULT matchResult;
    AmtMatchCorrespond(&candidates, pCtx->ActiveContacts,
                       NowQpc, pCtx->PerfFrequency.QuadPart,
                       &matchResult);

    // Gesture session FSM
    UCHAR aliveCount = 0;
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (!candidates.Candidates[ci].PalmLocal)
            aliveCount++;
    }

    AmtGestureSessionUpdate(&pCtx->GestureSession, aliveCount);
    BOOLEAN gestureThisFrame = AmtGestureIsMultiFingerFrame(aliveCount);

    
    // Drain deferred lift-offs
    AmtCoreDrainOverflow(pCtx, OutResult);

    // Phase A (lift): unmatched pool entries lift.
    // Gesture-tainted: defer kill on last finger; solo: kill immediately.

    for (UCHAR u = 0; u < matchResult.UnmatchedCount; u++) {
        size_t p = matchResult.UnmatchedPoolIndices[u];

        if (palmSuppressedFrame) {
            // The raw frame still reports a contact here - this finger
            // never actually left the pad, it just got classified as
            // palm this frame (or a prior frame, sticky PalmDetected).
            // Do NOT kill it: that would emit a real CONTACT_PHASE_UP
            // for a contact that's still physically down, and Windows'
            // PTP stack reads a fast DOWN->UP with no real movement as
            // a tap/double-tap. Instead freeze it in place - same
            // ContactID, same last-known position, TipSwitch=1 (still
            // "down" per protocol) but Confidence=0 so Windows discards
            // it for pointer/gesture purposes. Pool entry stays
            // CONTACT_ACTIVE untouched; it only gets a real UP once the
            // raw frame genuinely reports zero contacts, at which point
            // palmSuppressedFrame is FALSE again and this branch is
            // skipped, falling through to the normal kill path below.
            AmtCoreEmitContact(pCtx, OutResult, pCtx->ActiveContacts[p].ContactID,
                               pCtx->ActiveContacts[p].ReportX,
                               pCtx->ActiveContacts[p].ReportY,
                               CONTACT_PHASE_MOVE, FALSE);
            continue;
        }

        ULONG  oldId; USHORT oldX, oldY;

        if (pCtx->ActiveContacts[p].WasInGesture) {
            // Gesture-tainted: defer if fresh and last finger.
            if (pCtx->ActiveContacts[p].FramesAlive < MIN_CONTACT_LIFETIME_FRAMES
                && aliveCount == 0)
            {
                // Defer one frame for gesture recognizer.
                pCtx->ActiveContacts[p].FramesAlive++;

                AmtCoreEmitContact(pCtx, OutResult, pCtx->ActiveContacts[p].ContactID,
                                   pCtx->ActiveContacts[p].ReportX, pCtx->ActiveContacts[p].ReportY,
                                   CONTACT_PHASE_MOVE, TRUE);
                continue; // no lift-off this frame
            }

            // Gesture lift: not recorded in RecentLifts.
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
            // No AmtRecentLiftRecord here - intentional (Issue #4 fix).
            AmtCoreEmitContact(pCtx, OutResult, oldId, oldX, oldY, CONTACT_PHASE_UP, TRUE);

        } else {
            // Solo contact: kill immediately. palmSuppressedFrame is
            // always FALSE here - that case is handled above and
            // never falls through to this branch.
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
            AmtCoreEmitContact(pCtx, OutResult, oldId, oldX, oldY, CONTACT_PHASE_UP, TRUE);
        }
    }

    // NewIdentity (origin==0): lift old + birth new.
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (candidates.Candidates[ci].PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue;
        if (!matchResult.NewIdentity[ci]) continue;

        ULONG  oldId; USHORT oldX, oldY;
        if (pCtx->ActiveContacts[p].WasInGesture) {
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
            // Gesture lift: not recorded in RecentLifts.
        } else {
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            // Palm-suppressed: candidates.Count==0, loop won't execute.
            AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
        }

        AmtCoreEmitContact(pCtx, OutResult, oldId, oldX, oldY, CONTACT_PHASE_UP, TRUE);

        matchResult.CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;
    }

    // OPTIMIZATION: consolidated from 4 checkpoints (after NewIdentity, A.5,
    // Phase B, Phase C) to 2 - DBG-only either way (compiles to (VOID)0 in
    // retail), but halves the walk-the-whole-pool cost in debug builds
    // without losing meaningful phase isolation: this one now covers
    // Phase A (lift) + NewIdentity + Phase A.5 combined.
    // Phase A.5 (button-click forced rebirth): on the rising edge of the
    // integrated button, force a new ContactID onto every still-live,
    // pre-existing matched contact, in place, at its own current position.
    // This is the only sanctioned way to mint a new ContactID (ActiveContact.h:
    // "ContactID monotonic... the only permitted NEW_IDENTITY path is
    // Kill->Birth" or, here, an equivalent in-place rebind) - the pool slot
    // itself is NOT freed/reacquired, so WasInGesture/FramesAlive/HystX/Y/
    // ReportX/Y all carry across automatically, since the same physical
    // finger never actually left the pad. Pre-existing == LastSeenQpc != 0:
    // a contact birthed earlier in THIS frame (NewIdentity path above, or a
    // genuinely new touch in Phase B below) has no "old" cursor-latched
    // identity for Windows to be snapping against yet, so it's skipped.
    BOOLEAN rebindThisFrame[MAX_CONTACTS] = { 0 };

    if (buttonClickEdge) {
        for (UCHAR ci = 0; ci < candidates.Count; ci++) {
            if (candidates.Candidates[ci].PalmLocal) continue;

            size_t p = matchResult.CorrespondingPoolIndex[ci];
            if (p == MATCH_NO_CORRESPONDENCE) continue;
            if (pCtx->ActiveContacts[p].LastSeenQpc == 0) continue; // born this frame

            USHORT oldX = pCtx->ActiveContacts[p].ReportX;
            USHORT oldY = pCtx->ActiveContacts[p].ReportY;

            ULONG oldId;
            AmtContactRebindIdentity(pCtx->ActiveContacts, p, &pCtx->NextContactId, &oldId);
            AmtCoreEmitContact(pCtx, OutResult, oldId, oldX, oldY, CONTACT_PHASE_UP, TRUE);
            // Deliberately NOT AmtRecentLiftRecord'd - this isn't a real
            // lift and must not seed retap-smoothing for unrelated future
            // taps in the same area.

            rebindThisFrame[p] = TRUE;

            // AUDIT FIX: AmtContactRebindIdentity (unlike AmtContactBirth)
            // deliberately does NOT touch LastSeenQpc - it's an in-place
            // identity swap, not a birth. But Phase C below decides
            // DOWN-vs-MOVE purely from (LastSeenQpc == 0), and this slot's
            // LastSeenQpc still holds last frame's timestamp from before
            // the rebind. Left alone, Phase C would report the brand-new
            // ContactID as MOVE instead of DOWN - which defeats the whole
            // point of this rebind (Windows' PTP stack only routes a
            // contact through the non-snapping soft-tap path when it sees
            // TipSwitch go 0->1 for that ContactID). Force it to 0 here so
            // Phase C's justBorn check fires correctly; AmtContactUpdate
            // overwrites it with the real nowQpc a few lines later
            // regardless, and PendingFirstSample (untouched, already
            // FALSE) keeps hysteresis/deadzone tracking uninterrupted -
            // only the reported Phase flips, not the internal tracking.
            pCtx->ActiveContacts[p].LastSeenQpc = 0;

            // matchResult.CorrespondingPoolIndex[ci] stays == p: the same
            // pool slot now carries the new ContactID and Phase C below
            // updates/reports it normally, at this frame's live position.
        }
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // Phase B (birth): unmatched candidates birth new pool entries.
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];
        if (cand->PalmLocal) continue;
        if (matchResult.CorrespondingPoolIndex[ci] != MATCH_NO_CORRESPONDENCE)
            continue; // handled in Phase C

        size_t freeIdx = AmtContactPoolFindFree(pCtx->ActiveContacts);
        if (freeIdx == MAX_CONTACTS) {
            continue;
        }

        USHORT liftX, liftY;
        BOOLEAN looksLikeRetap =
            AmtRecentLiftFindNearby(&pCtx->RecentLifts, NowQpc,
                                    pCtx->PerfFrequency.QuadPart,
                                    cand->X, cand->Y, &liftX, &liftY);

        if (looksLikeRetap) {
            // RetapSeeded: seed survives first AmtContactUpdate.
            AmtContactBirthWithRetapSmoothing(
                pCtx->ActiveContacts, freeIdx, &pCtx->NextContactId,
                liftX, liftY, cand->SlotIndex);
        } else {
            AmtContactBirth(
                pCtx->ActiveContacts, freeIdx, &pCtx->NextContactId,
                cand->X, cand->Y, cand->SlotIndex);
        }

        // NOTE: WasInGesture is NOT decided here. Phase C below runs
        // immediately after for this exact same candidate (candidates
        // are walked again by index) and makes the real, final taint
        // decision - setting it here would just be overwritten. See the
        // AUDIT FIX comment in Phase C for the actual logic.

        matchResult.CorrespondingPoolIndex[ci] = freeIdx;
    }

    // Phase C (update / report): update once, report once.
    //
    // Pre-pass: snapshot, BEFORE the mutating loop below runs, (a) which
    // candidates are fresh births (LastSeenQpc == 0) and (b) each live
    // contact's WasInGesture as of the START of this frame. Needed so
    // the tail-overlap taint check further down sees a stable,
    // order-independent picture - reading pool state mid-loop would give
    // different answers depending on candidate iteration order, since
    // AmtContactUpdate (called per-candidate below) and the taint
    // assignment itself immediately mutate whichever contact was just
    // processed.
    BOOLEAN candidateJustBorn[PTP_MAX_CONTACT_POINTS]   = { 0 };
    BOOLEAN candidateWasTainted[PTP_MAX_CONTACT_POINTS] = { 0 };
    for (UCHAR pc = 0; pc < candidates.Count; pc++) {
        if (candidates.Candidates[pc].PalmLocal) continue;
        size_t pp = matchResult.CorrespondingPoolIndex[pc];
        if (pp == MATCH_NO_CORRESPONDENCE) continue;
        candidateJustBorn[pc]   = (pCtx->ActiveContacts[pp].LastSeenQpc == 0);
        candidateWasTainted[pc] = pCtx->ActiveContacts[pp].WasInGesture;
    }

    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];
        if (cand->PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue;

        BOOLEAN justBorn = candidateJustBorn[ci];

        // AUDIT FIX (tail-overlap false taint): gestureThisFrame alone
        // (aliveCount>=2 this frame) does NOT mean a contact born THIS
        // frame is a participant in a real multi-finger gesture. Sensor
        // scan cadence means a dying gesture's last finger can still be
        // present in this exact frame's raw candidates (matched,
        // continuing) at the same time a brand-new solo tap is born -
        // producing aliveCount==2 for one transient frame despite this
        // being two unrelated touches, not a co-starting gesture. That
        // used to taint the new tap with WasInGesture unconditionally,
        // routing an intended solo tap through the gesture-lift
        // defer/grace path (Phase A above) instead of an immediate kill
        // + RecentLifts record - silently eating taps right after any
        // multi-finger gesture release (2-finger scroll, 3-finger swipe,
        // etc), because the driver never distinguished gesture size or
        // type - only raw finger count this frame.
        //
        // Fix applies only to contacts born THIS frame (justBorn):
        // inherit the taint only if ANOTHER live candidate this frame
        // was NOT ALREADY tainted as of the start of this frame. An
        // untainted co-alive partner (whether it's also a fresh birth,
        // or an existing contact that simply hasn't joined a gesture
        // yet) is the real signature of a genuine gesture start - and
        // this correctly covers a multi-finger gesture whose fingers
        // touch down a frame or two apart, not just the exact same
        // frame (an earlier version of this fix required same-frame
        // co-birth, which wrongly failed to taint the second finger of
        // a staggered-start gesture that lifted again before its own
        // next Phase C pass). A co-alive partner that was ALREADY
        // tainted before this frame is, by construction, the tail of a
        // separate, already-established gesture - that's the actual
        // tail-overlap case, and must NOT cause the new birth to be
        // tainted.
        //
        // Pre-existing (non-justBorn) contacts are unaffected: they keep
        // getting (re-)tainted on every gestureThisFrame frame exactly
        // as before, since they are genuinely still part of whatever
        // gesture they started in - this only changes what a BIRTH
        // inherits.
        BOOLEAN shouldTaint = gestureThisFrame;

        if (shouldTaint && justBorn) {
            BOOLEAN otherCandidateUntainted = FALSE;
            for (UCHAR oc = 0; oc < candidates.Count; oc++) {
                if (oc == ci || candidates.Candidates[oc].PalmLocal) continue;
                if (matchResult.CorrespondingPoolIndex[oc] == MATCH_NO_CORRESPONDENCE)
                    continue;
                if (!candidateWasTainted[oc]) {
                    otherCandidateUntainted = TRUE;
                    break;
                }
            }

            shouldTaint = otherCandidateUntainted;
        }

        if (shouldTaint) {
            pCtx->ActiveContacts[p].WasInGesture = TRUE;
        }

        USHORT repX, repY;
        AmtContactUpdate(&pCtx->ActiveContacts[p], cand->X, cand->Y,
                         cand->SlotIndex, NowQpc,
                         (BOOLEAN)(aliveCount == 1), gestureThisFrame,
                         &repX, &repY);

        // AUDIT FIX (click Confidence false-negative): TipDropApplied
        // means "X/Y is stale/bridged," not "this isn't a real finger" -
        // but Phase C was reusing it as-is for Confident on EVERY report,
        // including a buttonClickEdge rebind's DOWN. A physical mechanical
        // click on this hardware momentarily flexes the pad and shrinks
        // the reported touch ellipse (Major/Minor), often dropping it
        // below AmtMatchCandidateTip's threshold right at the click - and
        // since a deliberate click is almost always thrown while the
        // finger is held still, that lands in the "isStationary" tip-drop
        // branch (Match.c) which sets TipDropApplied=1. That falsely
        // reported the freshly-rebound ContactID's DOWN as Confident=
        // FALSE, and Windows' PTP stack discards non-confident contacts
        // for pointer/click purposes - so the click never registered
        // unless the finger was lifted and touched again (a real birth,
        // on a later frame once the ellipse recovered above threshold).
        // A rebound contact is by definition an already-tracked, real
        // finger (identity swap only, per AmtContactRebindIdentity) - so
        // its Confidence must not be gated by the tip-threshold heuristic
        // meant for brand-new/continuing touch candidates.
        BOOLEAN reportConfident = rebindThisFrame[p]
            ? TRUE
            : (BOOLEAN)(cand->TipDropApplied == 0);

        AmtCoreEmitContact(pCtx, OutResult, pCtx->ActiveContacts[p].ContactID,
                           repX, repY,
                           justBorn ? CONTACT_PHASE_DOWN : CONTACT_PHASE_MOVE,
                           reportConfident);
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // Force-touch drag lockout. Recomputed every frame from the RAW
    // frame (same rationale as below: click-flex must not distort this
    // either), BEFORE the pressure check, so a press that has turned
    // into a drag can never still trip force-touch this same frame.
    if (!ButtonDown) {
        pCtx->ForceTouchAnchorValid = FALSE;
        pCtx->ForceTouchDragLockout = FALSE;
    } else {
        if (buttonClickEdge && RawFrame->ContactCount > 0) {
            pCtx->ForceTouchAnchorX     = RawFrame->Contacts[0].X;
            pCtx->ForceTouchAnchorY     = RawFrame->Contacts[0].Y;
            pCtx->ForceTouchAnchorValid = TRUE;
        }

        if (pCtx->ForceTouchAnchorValid) {
            for (UCHAR fi = 0; fi < RawFrame->ContactCount; fi++) {
                INT dx = (INT)RawFrame->Contacts[fi].X - (INT)pCtx->ForceTouchAnchorX;
                INT dy = (INT)RawFrame->Contacts[fi].Y - (INT)pCtx->ForceTouchAnchorY;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx > FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE ||
                    dy > FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE) {
                    pCtx->ForceTouchDragLockout = TRUE;
                    break;
                }
            }
        }
    }

    // Peak raw pressure this frame - shared by the click arbitration and
    // force-touch checks below (same click-flex rationale as the old
    // per-block loops: use the RAW frame, not the matched/palm-filtered
    // candidates).
    USHORT framePeakPressure = 0;
    for (UCHAR fi = 0; fi < RawFrame->ContactCount; fi++) {
        if (RawFrame->Contacts[fi].Pressure > framePeakPressure)
            framePeakPressure = RawFrame->Contacts[fi].Pressure;
    }

    // Click arbitration: force-touch vs ordinary hard-tap. Decides BEFORE
    // the force-touch check below so both see the same, already-updated
    // state this frame. Once the button is held, this press stays
    // CLICK_ARBITRATION_PENDING until one of four things happens:
    //   - the drag lockout trips (finger has moved past
    //     FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE from the button-down anchor,
    //     e.g. dragging a window) -> HARD_TAP, unconditionally, even if
    //     pressure ALSO crosses the force-touch threshold in this exact
    //     same frame. Movement wins over pressure: a press that's moving
    //     is a drag, full stop, regardless of how hard it's pressed.
    //     Checked FIRST, ahead of the pressure check below, specifically
    //     for that same-frame race;
    //   - pressure crosses FORCE_TOUCH_PRESSURE_THRESHOLD (and the drag
    //     lockout has NOT tripped) -> FORCE_TOUCH, the ordinary click is
    //     suppressed for the rest of the press;
    //   - pressure falls CLICK_ARBITRATION_PRESSURE_HYSTERESIS units or
    //     more below the highest pressure seen so far this press, before
    //     ever reaching the threshold -> the press has peaked and is on
    //     its way back down -> HARD_TAP, immediately. Compared against
    //     the running PEAK rather than the previous frame's raw sample:
    //     the sensor is noisy (252, 250, 251, 252, 251...), so a single
    //     frame-to-frame dip is not a reliable "it's receding" signal -
    //     only falling meaningfully below the best value seen is;
    //   - none of the above within CLICK_ARBITRATION_TIMEOUT_MS (pressure
    //     climbing slowly or just wobbling) -> give up waiting -> HARD_TAP.
    // The decision then latches for the remainder of the press (reset to
    // IDLE only on button release), so a later pressure swing OR later
    // movement can't flip it - once FORCE_TOUCH is decided, moving the
    // finger while still pressing is a right-click-drag (e.g. dragging
    // out a selection) and must NOT cancel the force touch; only the
    // pressure dropping back down (or the button releasing) ends it -
    // see forceTouchNow below, which is intentionally NOT gated on the
    // drag lockout once FORCE_TOUCH is already latched.
    if (!ButtonDown) {
        pCtx->ClickArbitrationState = CLICK_ARBITRATION_IDLE;
    } else {
        if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_IDLE) {
            pCtx->ClickArbitrationState         = CLICK_ARBITRATION_PENDING;
            pCtx->ClickArbitrationStartQpc      = NowQpc;
            pCtx->ClickArbitrationPeakPressure  = framePeakPressure;
        }
        if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_PENDING) {
            if (framePeakPressure > pCtx->ClickArbitrationPeakPressure)
                pCtx->ClickArbitrationPeakPressure = framePeakPressure;

            INT dropFromPeak = (INT)pCtx->ClickArbitrationPeakPressure -
                                (INT)framePeakPressure;

            if (pCtx->ForceTouchDragLockout) {
                // Movement beats pressure, even in a same-frame tie -
                // checked before the threshold test below on purpose.
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
            } else if (framePeakPressure > FORCE_TOUCH_PRESSURE_THRESHOLD) {
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_FORCE_TOUCH;
            } else if (dropFromPeak >= CLICK_ARBITRATION_PRESSURE_HYSTERESIS) {
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
            } else {
                LONGLONG elapsedTicks = NowQpc - pCtx->ClickArbitrationStartQpc;
                LONGLONG timeoutTicks = (pCtx->PerfFrequency.QuadPart > 0)
                    ? (pCtx->PerfFrequency.QuadPart * CLICK_ARBITRATION_TIMEOUT_MS) / 1000
                    : 0; // no usable clock - fail open to a plain click below
                if (elapsedTicks >= timeoutTicks) {
                    pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
                }
            }
        }
        // else: HARD_TAP or FORCE_TOUCH already latched - hold it.
    }

    *OutButtonClickReport =
        (pCtx->ClickArbitrationState == CLICK_ARBITRATION_HARD_TAP);

    // Force-touch: once click arbitration has latched FORCE_TOUCH for
    // this press, it stays engaged unconditionally until the button is
    // released - it does NOT re-check pressure frame-to-frame anymore.
    // Pressure naturally wobbles while the finger holds/drags (sensor
    // noise, grip changes), and re-testing it here every frame caused a
    // spurious re-trigger: a momentary dip back under the threshold
    // dropped ForceTouchActive (sending a synthetic right-click-UP),
    // then the next frame's recovery sent a right-click-DOWN again -
    // a second, unwanted force-touch trigger mid-press. A real re-trigger
    // must only happen after a genuine release (button up resets
    // ClickArbitrationState to IDLE, so the next press starts PENDING
    // and must cross the threshold again on its own).
    BOOLEAN forceTouchNow =
        ButtonDown && (pCtx->ClickArbitrationState == CLICK_ARBITRATION_FORCE_TOUCH);


    *OutForceTouchDownEdge = forceTouchNow && !pCtx->ForceTouchActive;
    *OutForceTouchUpEdge   = !forceTouchNow && pCtx->ForceTouchActive;
    pCtx->ForceTouchActive = forceTouchNow;
}
