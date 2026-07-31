// PTPCore.c - Frame orchestration: Phase A (lift) -> Phase B (birth) -> Phase C (update).

#include "Driver.h"
#include "PTPCore.h"
#include "PTPCore.tmh"
#include "ActiveContact.h"
#include "Match.h"

// Movement past this distance (normalized units - same coordinate space
// as RETAP_MAX_DISTANCE in ActiveContact.h) from the button-down anchor
// latches ForceTouchDragLockout for the rest of the press. Unlike before,
// this is no longer just a guard against ENTERING force touch - it can
// also retroactively CANCEL a force touch that has already been decided
// (see the click arbitration block below): a press that has moved this
// far is a window/file/selection drag, full stop, no matter how hard it
// was pressed at any point during the hold.
#define FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE 70

// How long PTPCore will wait, once the button goes down, for the press
// to either cross FORCE_TOUCH_PRESSURE_THRESHOLD or start receding
// before giving up and committing it as an ordinary click. "Some time"
// per the design note - tunable.
#define CLICK_ARBITRATION_TIMEOUT_MS 60

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

// AUDIT FIX (duplicate ContactID within one report - overflow x Phase C
// across a frame boundary): a queued Phase C DOWN/MOVE overflow entry
// (see AmtCoreEmitContact above) belongs to a contact that is, by
// construction, still CONTACT_ACTIVE - it only ever lands in the
// overflow queue because the frame ran out of room, never because the
// contact died. Phase A/NewIdentity/A.5 UP overflow entries are safe to
// drain unconditionally because the underlying pool slot is always
// killed/freed in the very same frame that queued them - by the next
// frame its ContactID no longer exists in the pool, so nothing else
// will ever report it again. A Phase C DOWN/MOVE entry has no such
// guarantee: the physical finger never left the pad, so if it's still
// matched next frame, Phase C further down in THIS SAME function call
// will independently produce a brand-new, current-position report for
// that exact ContactID - draining the stale one first would put two
// entries carrying the same ContactID into one OutResult, tripping
// AmtReportCheckInvariants (Interrupt.c, DBG builds) and handing
// Windows an ambiguous report in retail. See AmtPoolHoldsContactID.
static BOOLEAN
AmtPoolHoldsContactID(
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _In_                     ULONG                  ContactID
)
{
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        if (Pool[p].State == CONTACT_ACTIVE && Pool[p].ContactID == ContactID)
            return TRUE;
    }
    return FALSE;
}

// Drain deferred reports from previous frame(s).
static VOID
AmtCoreDrainOverflow(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _Inout_ PTP_CORE_FRAME* OutResult
)
{
    // Compacted write cursor: entries not resolved this call (still no
    // room, not yet superseded) get shifted down to keep the queue
    // dense. keep <= k at all times, so writing Overflow*[keep] before
    // reading Overflow*[k] on a later iteration never clobbers unread
    // data - safe without a temp/RtlMoveMemory batch.
    UCHAR keep = 0;

    for (UCHAR k = 0; k < pCtx->OverflowCount; k++) {
        ULONG id = pCtx->OverflowContactID[k];

        if (AmtPoolHoldsContactID(pCtx->ActiveContacts, id)) {
            // Superseded: this ContactID is still alive in the pool, so
            // Phase A or Phase C is guaranteed to emit a fresh, correctly
            // timestamped report for it later this very frame. Drop the
            // stale queued one outright rather than drain it - draining
            // would duplicate the ContactID in OutResult (see comment
            // above); dropping just means the finger's position for the
            // frame that originally overflowed is superseded by the
            // very next frame's report instead of being shown a frame
            // late, which is what should happen anyway.
            continue;
        }

        if (OutResult->ContactCount < PTP_MAX_CONTACT_POINTS) {
            PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
            outC->ContactID   = pCtx->OverflowContactID[k];
            outC->X           = pCtx->OverflowX[k];
            outC->Y           = pCtx->OverflowY[k];
            outC->Phase       = pCtx->OverflowPhase[k];
            outC->Confident   = pCtx->OverflowConfident[k];
            outC->PalmSuspect = FALSE;
            OutResult->ContactCount++;
            continue;
        }

        // Orphaned (ContactID no longer live) but still no room this
        // frame either - keep it queued for a later frame.
        pCtx->OverflowContactID[keep] = pCtx->OverflowContactID[k];
        pCtx->OverflowX[keep]         = pCtx->OverflowX[k];
        pCtx->OverflowY[keep]         = pCtx->OverflowY[k];
        pCtx->OverflowPhase[keep]     = pCtx->OverflowPhase[k];
        pCtx->OverflowConfident[keep] = pCtx->OverflowConfident[k];
        keep++;
    }

    pCtx->OverflowCount = keep;
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

    // Per-frame multi-finger check (feeds the taint gate below)
    UCHAR aliveCount = 0;
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (!candidates.Candidates[ci].PalmLocal)
            aliveCount++;
    }

    // Per-frame multi-finger taint gate: >=2 fingers concurrently down
    // this exact frame (see removed Gesture.c/h - this one comparison
    // was its entire remaining content once the dead session-sticky
    // field was cut, so it's inlined here instead of kept as a module).
    BOOLEAN gestureThisFrame = (aliveCount >= 2);

    // BUG FIX (phantom UP from the bottom/edge hard cutoff): AmtMatchCorrespond
    // never matches a PALM_LOCAL candidate to a pool entry (Match.c), so a
    // contact that's still actively tracked and simply drags into the edge
    // dead zone looks, to Phase A below, exactly like a genuine unmatched
    // (lifted) contact - producing a real CONTACT_PHASE_UP while the finger
    // is still down. Same failure class palmSuppressedFrame already exists
    // to prevent for PALM_LARGE. Build the set of pool slots that have a
    // PalmLocal candidate this frame (matched by LastSlotHint, same
    // technique Match.c's tip-drop bridge already uses) so Phase A can
    // freeze them instead of killing them.
    BOOLEAN palmLocalFrozen[MAX_CONTACTS] = { 0 };
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (!candidates.Candidates[ci].PalmLocal)
            continue;

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (pCtx->ActiveContacts[p].State == CONTACT_ACTIVE &&
                pCtx->ActiveContacts[p].LastSlotHint == candidates.Candidates[ci].SlotIndex)
            {
                palmLocalFrozen[p] = TRUE;
                break;
            }
        }
    }

    // Drain deferred lift-offs
    AmtCoreDrainOverflow(pCtx, OutResult);

    // Phase A (lift): unmatched pool entries lift.
    // Gesture-tainted: defer kill on last finger; solo: kill immediately.

    for (UCHAR u = 0; u < matchResult.UnmatchedCount; u++) {
        size_t p = matchResult.UnmatchedPoolIndices[u];

        if (palmSuppressedFrame || palmLocalFrozen[p]) {
            // The raw frame still reports a contact here - this finger
            // never actually left the pad, it just got classified as
            // palm (PALM_LARGE) or edge/bottom dead zone (PALM_LOCAL)
            // this frame (or, for PALM_LARGE, a prior frame via sticky
            // PalmDetected). Do NOT kill it: that would emit a real
            // CONTACT_PHASE_UP for a contact that's still physically
            // down, and Windows' PTP stack reads a fast DOWN->UP with no
            // real movement as a tap/double-tap (or, mid-drag, as an
            // unwanted drag-end). Instead freeze it in place - same
            // ContactID, same last-known position, TipSwitch=1 (still
            // "down" per protocol) but Confidence=0 so Windows discards
            // it for pointer/gesture purposes. Pool entry stays
            // CONTACT_ACTIVE untouched; it only gets a real UP once the
            // raw frame genuinely reports zero contacts for this slot, at
            // which point both palmSuppressedFrame and palmLocalFrozen[p]
            // are FALSE again and this branch is skipped, falling through
            // to the normal kill path below.
            //
            // AUDIT FIX (stale LastSeenQpc -> spurious time-reject on
            // dead-zone/palm exit): this branch intentionally skips
            // AmtContactUpdate (deadzone/EMA must not run on a frozen
            // contact), but AmtContactUpdate is also the ONLY place
            // LastSeenQpc is ever refreshed (Activecontact.c). Left
            // untouched, a contact frozen here for longer than
            // MATCH_MAX_TIME_DELTA_100NS (Match.h, 150ms) - e.g. a palm
            // resting on the lower edge, or a finger parked in the
            // PALM_LOCAL dead zone - accumulates a stale LastSeenQpc even
            // though it's still visibly present every single frame. The
            // moment it re-qualifies as a normal candidate again (palm
            // lifts, or the finger slides back into the live area),
            // AmtMatchCorrespond's time-domain rejection (Match.c) sees
            // "last seen >150ms ago" and rejects the otherwise-correct
            // spatial match - killing this ContactID for real and
            // birthing a brand-new one, for a finger that in fact never
            // left the pad. Refresh LastSeenQpc directly (NOT via
            // AmtContactUpdate, which would also run deadzone/EMA against
            // a position this contact isn't actually reporting live) so
            // "last seen" tracks "last visibly present," matching what
            // this branch is actually claiming by staying CONTACT_ACTIVE.
            pCtx->ActiveContacts[p].LastSeenQpc = NowQpc;

            AmtCoreEmitContact(pCtx, OutResult, pCtx->ActiveContacts[p].ContactID,
                               pCtx->ActiveContacts[p].ReportX,
                               pCtx->ActiveContacts[p].ReportY,
                               CONTACT_PHASE_MOVE, FALSE);
            continue;
        }

        ULONG  oldId; USHORT oldX, oldY;

        if (pCtx->ActiveContacts[p].WasInGesture) {
            // Gesture-tainted: lift immediately, no defer. The
            // MIN_CONTACT_LIFETIME_FRAMES fake-MOVE-then-UP deferral was
            // removed here (previously gated on FramesAlive < 4 &&
            // aliveCount == 0, i.e. "fresh last finger of a gesture
            // session") because it delayed the real UP by up to 4 frames
            // (~33ms @ 120Hz) - exactly the artificial-timing problem
            // Issue #2 already fixed for solo taps below; it had just
            // never been removed from this branch. Symptoms traced
            // directly to this: delayed 3-finger swipe completion,
            // 2-finger soft tap intermittently not registering.
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
    //
    // AUDIT FIX (rebind mistaken for birth in the taint pre-pass): Phase
    // A.5 above forces LastSeenQpc=0 on a button-click rebind so Phase C
    // reports DOWN instead of MOVE for the new ContactID (see the AUDIT
    // FIX comment there) - that part is correct and must stay. But this
    // pre-pass was ALSO using the very same (LastSeenQpc==0) test to
    // decide whether the tail-overlap taint check below applies, so a
    // rebind - an already-live, physically continuous finger that simply
    // got a new ContactID - was indistinguishable here from a genuine
    // brand-new touch. That routed a rebound contact through the
    // conditional "inherit taint only from an untainted partner" logic
    // instead of the unconditional "pre-existing contacts always get
    // (re-)tainted on a gestureThisFrame frame" rule the comment below
    // documents - so a finger that was already mid-gesture could lose
    // its WasInGesture taint on the exact frame it got rebound (e.g. a
    // button click fired mid multi-finger gesture), if its only "other"
    // candidate this frame happened to already be tainted (the routine
    // tail-overlap case this same check exists to filter for births).
    // candidateIsTrueBirth separates the two: TRUE only for an actual
    // Phase B pool allocation this frame, FALSE for a rebind (even
    // though rebind also has LastSeenQpc==0). justBorn (DOWN vs MOVE,
    // used further below) is intentionally left alone - unaffected by
    // this fix.
    BOOLEAN candidateJustBorn[PTP_MAX_CONTACT_POINTS]    = { 0 };
    BOOLEAN candidateWasTainted[PTP_MAX_CONTACT_POINTS]  = { 0 };
    BOOLEAN candidateIsTrueBirth[PTP_MAX_CONTACT_POINTS] = { 0 };
    for (UCHAR pc = 0; pc < candidates.Count; pc++) {
        if (candidates.Candidates[pc].PalmLocal) continue;
        size_t pp = matchResult.CorrespondingPoolIndex[pc];
        if (pp == MATCH_NO_CORRESPONDENCE) continue;
        candidateJustBorn[pc]    = (pCtx->ActiveContacts[pp].LastSeenQpc == 0);
        candidateWasTainted[pc]  = pCtx->ActiveContacts[pp].WasInGesture;
        candidateIsTrueBirth[pc] = candidateJustBorn[pc] && !rebindThisFrame[pp];
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
        // Fix applies only to candidates that are a TRUE fresh pool
        // birth this frame (candidateIsTrueBirth, NOT justBorn - see the
        // AUDIT FIX comment on the pre-pass above for why a button-click
        // rebind must NOT take this branch): inherit the taint only if
        // ANOTHER live candidate this frame was NOT ALREADY tainted as
        // of the start of this frame. An untainted co-alive partner
        // (whether it's also a fresh birth, or an existing contact that
        // simply hasn't joined a gesture yet) is the real signature of a
        // genuine gesture start - and this correctly covers a
        // multi-finger gesture whose fingers touch down a frame or two
        // apart, not just the exact same frame (an earlier version of
        // this fix required same-frame co-birth, which wrongly failed to
        // taint the second finger of a staggered-start gesture that
        // lifted again before its own next Phase C pass). A co-alive
        // partner that was ALREADY tainted before this frame is, by
        // construction, the tail of a separate, already-established
        // gesture - that's the actual tail-overlap case, and must NOT
        // cause the new birth to be tainted.
        //
        // Pre-existing (non-true-birth) contacts are unaffected - this
        // now correctly includes button-click rebinds, not just
        // ordinary continuing contacts: they keep getting (re-)tainted
        // on every gestureThisFrame frame exactly as before, since they
        // are genuinely still part of whatever gesture they started in -
        // this only changes what a BIRTH inherits.
        BOOLEAN shouldTaint = gestureThisFrame;

        if (shouldTaint && candidateIsTrueBirth[ci]) {
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
                         cand->Major, cand->Minor, cand->Pressure,
                         cand->SlotIndex, NowQpc, pCtx->PerfFrequency.QuadPart,
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
        // AUDIT FIX: previously gated on (buttonClickEdge && ContactCount>0),
        // so if the exact click-edge frame happened to report zero raw
        // contacts - plausible on this hardware, since a mechanical click
        // is already documented elsewhere in this file to momentarily
        // flex the pad and shrink/drop the reported touch ellipse - the
        // anchor was NEVER armed for the rest of that press (this branch
        // only ever ran on the single buttonClickEdge frame). With no
        // anchor, the drag-lockout distance check below never runs, so a
        // press that turns into a drag could never be reclassified out of
        // force-touch eligibility for that entire press. Arming on
        // "!ForceTouchAnchorValid" instead of "buttonClickEdge" arms on
        // the FIRST frame with contacts after button-down (whether that's
        // the edge frame itself or a frame or two later), and - since
        // ForceTouchAnchorValid then stays TRUE until release - still
        // only ever arms once per press, at the earliest available data.
        if (!pCtx->ForceTouchAnchorValid && RawFrame->ContactCount > 0) {
            pCtx->ForceTouchAnchorX     = RawFrame->Contacts[0].X;
            pCtx->ForceTouchAnchorY     = RawFrame->Contacts[0].Y;
            pCtx->ForceTouchAnchorValid = TRUE;
        }

        if (pCtx->ForceTouchAnchorValid) {
            // BUG FIX: track only the ONE finger nearest the anchor, not
            // every raw contact. RawFrame->Contacts[] is packed in sensor
            // scan order this frame, not by a stable per-finger slot - so
            // testing ALL contacts against a single-finger anchor meant an
            // unrelated second finger landing elsewhere on the pad (resting
            // the hand, a stray touch) while the button was held could trip
            // the lockout purely because IT was far from the anchor, even
            // though the actual pressing finger never moved. Nearest-match
            // each frame instead: only the contact closest to the anchor is
            // compared against the threshold.
            LONG bestDistSq = -1;
            INT  bestDx = 0, bestDy = 0;
            for (UCHAR fi = 0; fi < RawFrame->ContactCount; fi++) {
                INT dx = (INT)RawFrame->Contacts[fi].X - (INT)pCtx->ForceTouchAnchorX;
                INT dy = (INT)RawFrame->Contacts[fi].Y - (INT)pCtx->ForceTouchAnchorY;
                LONG distSq = (LONG)dx * dx + (LONG)dy * dy;
                if (bestDistSq < 0 || distSq < bestDistSq) {
                    bestDistSq = distSq;
                    bestDx     = dx;
                    bestDy     = dy;
                }
            }

            if (bestDistSq >= 0) {
                INT adx = (bestDx < 0) ? -bestDx : bestDx;
                INT ady = (bestDy < 0) ? -bestDy : bestDy;
                if (adx > FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE ||
                    ady > FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE) {
                    pCtx->ForceTouchDragLockout = TRUE;
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
    //   - pressure crosses FORCE_TOUCH_PRESSURE_THRESHOLD (and the drag
    //     lockout has NOT tripped) -> FORCE_TOUCH, the ordinary click is
    //     suppressed for the rest of the press - UNLESS a later frame
    //     trips the drag lockout, see below;
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
    //
    // REWORK (force-touch-vs-drag): the drag lockout (movement past
    // FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE from the button-down anchor) is
    // now checked on EVERY frame, ahead of everything else, and applies
    // even to a press that has already latched FORCE_TOUCH - not just
    // during PENDING. A genuine force touch is only ever confirmed if the
    // finger never moves past the lockout distance for the WHOLE press,
    // right up to release; move too far at any point - before pressure
    // crosses the threshold, after it crosses, or even after force touch
    // has already fired - and this press is unconditionally a HARD_TAP
    // (window/file/selection drag) instead. This is a one-way downgrade:
    // once HARD_TAP, nothing later in the same press (pressure or
    // distance) can turn it back into FORCE_TOUCH. Reset to IDLE only on
    // button release, so the next press re-arms and must earn FORCE_TOUCH
    // again on its own.
    if (!ButtonDown) {
        pCtx->ClickArbitrationState = CLICK_ARBITRATION_IDLE;
    } else {
        if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_IDLE) {
            pCtx->ClickArbitrationState         = CLICK_ARBITRATION_PENDING;
            pCtx->ClickArbitrationStartQpc      = NowQpc;
            pCtx->ClickArbitrationPeakPressure  = framePeakPressure;
        }

        if (pCtx->ForceTouchDragLockout) {
            // Movement past the lockout distance always wins, this frame
            // or any later one - including retroactively cancelling a
            // FORCE_TOUCH decision already made earlier this same press.
            pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
        } else if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_PENDING) {
            if (framePeakPressure > pCtx->ClickArbitrationPeakPressure)
                pCtx->ClickArbitrationPeakPressure = framePeakPressure;

            INT dropFromPeak = (INT)pCtx->ClickArbitrationPeakPressure -
                                (INT)framePeakPressure;

            if (framePeakPressure > FORCE_TOUCH_PRESSURE_THRESHOLD) {
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
        // else: FORCE_TOUCH already latched and drag lockout hasn't
        // tripped (checked above) - hold it. HARD_TAP is likewise held
        // (no branch flips it back).
    }

    *OutButtonClickReport =
        (pCtx->ClickArbitrationState == CLICK_ARBITRATION_HARD_TAP);

    // Force-touch: once click arbitration has latched FORCE_TOUCH for
    // this press, it stays engaged until the button is released OR the
    // drag lockout retroactively downgrades ClickArbitrationState to
    // HARD_TAP above (real window/file drag) - it does NOT re-check
    // pressure frame-to-frame anymore. Pressure naturally wobbles while
    // the finger holds/drags (sensor noise, grip changes), and re-testing
    // pressure here every frame caused a spurious re-trigger: a momentary
    // dip back under the threshold dropped ForceTouchActive (sending a
    // synthetic right-click-UP), then the next frame's recovery sent a
    // right-click-DOWN again - a second, unwanted force-touch trigger
    // mid-press. A real re-trigger must only happen after a genuine
    // release (button up resets ClickArbitrationState to IDLE, so the
    // next press starts PENDING and must cross the threshold again on
    // its own) - the drag-lockout downgrade is the one deliberate
    // exception, and it falls straight out of ClickArbitrationState no
    // longer reading FORCE_TOUCH below, same as any other HARD_TAP.
    BOOLEAN forceTouchNow =
        ButtonDown && (pCtx->ClickArbitrationState == CLICK_ARBITRATION_FORCE_TOUCH);


    *OutForceTouchDownEdge = forceTouchNow && !pCtx->ForceTouchActive;
    *OutForceTouchUpEdge   = !forceTouchNow && pCtx->ForceTouchActive;
    pCtx->ForceTouchActive = forceTouchNow;
}