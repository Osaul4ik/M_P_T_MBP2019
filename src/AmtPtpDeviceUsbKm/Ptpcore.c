// PTPCore.c - Frame orchestration: Phase A (lift) -> Phase B (birth) -> Phase C (update).

#include "Driver.h"
#include "PTPCore.h"
#include "PTPCore.tmh"
#include "ActiveContact.h"
#include "Match.h"

// Movement past this distance (normalized units - same coordinate space
// as RETAP_MAX_DISTANCE in ActiveContact.h) from the button-down anchor
// latches ForceTouchDragLockout for the rest of the press: once the user
// is visibly dragging (e.g. moving a window) after a hard tap, a deeper
// press must not fire a synthetic right-click on top of that drag.
#define FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE 60

// How long PTPCore will let a press's ordinary click report stay
// withheld while pressure is still below FORCE_TOUCH_PRESSURE_THRESHOLD,
// before giving up on it ever crossing and reporting the click live.
// Bounds the delay for long, pressure-quiet holds (click-and-hold with
// no drag) - without this, such a hold would never get reported until
// release, no matter how long it's held. A real force touch ramps past
// the threshold well under this window in practice, so it rarely even
// gets the chance to elapse for a genuine force-touch press. Once
// pressure DOES cross the threshold, this timer no longer applies - see
// the AUDIT note above the arbitration block below. Tunable.
#define CLICK_ARBITRATION_GRACE_MS 60

// BUG FIX (single-frame noise falsely tainting a solo tap/double-tap as
// gesture): see GestureCandidateFrames in ActiveContact.h. Number of
// CONSECUTIVE qualifying frames (gestureThisFrame, post tail-overlap
// filtering below) required before WasInGesture actually latches. 2 is
// the minimum that filters a one-frame blip while still tainting a real
// 2(+)-finger gesture on its 2nd live frame - imperceptible for gestures,
// which run for many frames, but decisive for a single noisy sample.
#define GESTURE_TAINT_DEBOUNCE_FRAMES 2

// BUG FIX (spurious full-pool rebind from summed multi-finger force):
// this hardware's integrated-button bit is a raw firmware click-force
// threshold computed over TOTAL pressure across all fingers, not a true
// mechanical switch - confirmed via the BTN raw diagnostic in
// Interrupt.c: a clean, stable byte (not a misread offset) that flips
// to 1 for a handful of frames when a second finger lands on an
// already-resting first finger, with no felt click and no intent to
// click. A single qualifying frame used to be enough to fire
// buttonClickEdge and force-rebind every active contact's ContactID
// (Phase A.5 below) - which is exactly what a real click needs (Windows'
// anti-jitter snap must be routed around on the very frame the click
// lands), but also fires on this kind of momentary, unintended
// threshold crossing. Require the raw bit to hold for this many
// CONSECUTIVE frames before honoring it as a real click-edge - matches
// the same debounce pattern as GESTURE_TAINT_DEBOUNCE_FRAMES above. 3
// frames (~24ms at this hardware's ~8ms cadence) comfortably clears any
// deliberate click, which is held for tens of ms at minimum, while
// filtering the 1-2 frame blips seen in the repro log.
#define BUTTON_CLICK_DEBOUNCE_FRAMES 3

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
    // BUG FIX (spurious buttonClickEdge): debounce the RAW button bit
    // itself, before edge detection - see BUTTON_CLICK_DEBOUNCE_FRAMES.
    // ButtonDown must hold for several consecutive frames before it's
    // treated as "the button is down" for THIS purpose; debouncedButtonDown
    // (not the raw ButtonDown) is what PrevButtonClicked/buttonClickEdge
    // are computed from, so a momentary threshold crossing never reaches
    // Phase A.5's full-pool rebind. Deliberately narrow: force-touch
    // arbitration/drag-lockout further below stays on the RAW ButtonDown
    // (see their own "recomputed every frame from the RAW frame" comments)
    // - that logic already has its own pressure-based gating and a several-
    // frame delay there would just add latency to a real force-touch press
    // without fixing anything this bug report is about.
    if (ButtonDown) {
        if (pCtx->ButtonDebounceFrames < 255)
            pCtx->ButtonDebounceFrames++;
    } else {
        pCtx->ButtonDebounceFrames = 0;
    }
    BOOLEAN debouncedButtonDown =
        pCtx->ButtonDebounceFrames >= BUTTON_CLICK_DEBOUNCE_FRAMES;

    BOOLEAN buttonClickEdge = debouncedButtonDown && !pCtx->PrevButtonClicked;
    pCtx->PrevButtonClicked = debouncedButtonDown;

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
    AmtMatchCorrespond(&candidates, pCtx->ActiveContacts, pCtx->DeviceInfo,
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

    // BUG FIX (fast multi-finger tap misread as single-tap/extra-tap):
    // real fingers almost never touch down in the exact same raw frame -
    // there's typically a 1-3 frame stagger between births even for a
    // single deliberate N-finger tap. The Phase A defer/grace check below
    // used to gate purely on EACH contact's own FramesAlive vs
    // MIN_CONTACT_LIFETIME_FRAMES. For a FAST tap (total contact
    // lifetime under that threshold for at least one finger - the common
    // case, since a fast tap is short by definition), the finger that
    // was born earlier reaches the threshold and gets a real
    // CONTACT_PHASE_UP sooner than a later-born partner still stuck in
    // the defer branch reporting CONTACT_PHASE_MOVE ("still down"). That
    // artificially spreads the two lift-off reports across different
    // frames - purely a driver timing artifact, not physical reality -
    // which is enough for Windows' PTP tap-arity classifier to
    // occasionally miscount the gesture (reads it as a lone soft tap, or
    // as extra separate taps once combined with retap/recent-lift
    // bookkeeping).
    //
    // Fix: when a whole gesture releases at once (aliveCount == 0 this
    // frame - see below), don't gate each contact's defer decision on its
    // OWN FramesAlive. Compute the MINIMUM FramesAlive across every
    // still-tainted (WasInGesture) contact that's unmatched this exact
    // frame (i.e. the whole group lifting together) and gate all of them
    // on that shared value instead. This makes every member of the group
    // cross the MIN_CONTACT_LIFETIME_FRAMES gate on the SAME frame,
    // regardless of how staggered their individual births were, so their
    // CONTACT_PHASE_UP reports land together too.
    UCHAR gestureGroupMinFramesAlive = 0xFF;
    if (aliveCount == 0) {
        for (UCHAR ug = 0; ug < matchResult.UnmatchedCount; ug++) {
            size_t pg = matchResult.UnmatchedPoolIndices[ug];
            // Skip contacts that are frozen (palm/dead-zone) this frame -
            // they don't actually lift here (see the palmSuppressedFrame
            // / palmLocalFrozen branch below), so they aren't part of
            // this gesture's release-together group and must not drag
            // the shared minimum down.
            if (palmSuppressedFrame || palmLocalFrozen[pg])
                continue;
            if (pCtx->ActiveContacts[pg].WasInGesture &&
                pCtx->ActiveContacts[pg].FramesAlive < gestureGroupMinFramesAlive)
            {
                gestureGroupMinFramesAlive = pCtx->ActiveContacts[pg].FramesAlive;
            }
        }
    }

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
            // Gesture-tainted: defer if fresh and last finger.
            //
            // AUDIT FIX (staggered-birth tap release skew): gate on the
            // whole gesture group's shared minimum FramesAlive
            // (gestureGroupMinFramesAlive, computed above), NOT this
            // contact's own FramesAlive - see the comment above the
            // computation for why. Every tainted contact unmatched this
            // frame shares the same gate value, so they defer (or don't)
            // in lockstep instead of drifting apart by however many
            // frames their births happened to be staggered.
            if (gestureGroupMinFramesAlive < MIN_CONTACT_LIFETIME_FRAMES
                && aliveCount == 0)
            {
                // Defer one frame for gesture recognizer.
                pCtx->ActiveContacts[p].FramesAlive++;

                DbgPrint("[AmtPtp] DEFER pool=%Iu id=%lu framesAlive=%u groupMin=%u qpc=%I64d\n",
                         p, pCtx->ActiveContacts[p].ContactID,
                         pCtx->ActiveContacts[p].FramesAlive,
                         gestureGroupMinFramesAlive, NowQpc);

                AmtCoreEmitContact(pCtx, OutResult, pCtx->ActiveContacts[p].ContactID,
                                   pCtx->ActiveContacts[p].ReportX, pCtx->ActiveContacts[p].ReportY,
                                   CONTACT_PHASE_MOVE, TRUE);
                continue; // no lift-off this frame
            }

            // Gesture lift: not recorded in RecentLifts.
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
            // No AmtRecentLiftRecord here - intentional (Issue #4 fix).
            DbgPrint("[AmtPtp] UP gesture-tainted id=%lu X=%u Y=%u qpc=%I64d\n",
                     oldId, oldX, oldY, NowQpc);
            AmtCoreEmitContact(pCtx, OutResult, oldId, oldX, oldY, CONTACT_PHASE_UP, TRUE);

        } else {
            // Solo contact: kill immediately. palmSuppressedFrame is
            // always FALSE here - that case is handled above and
            // never falls through to this branch.
            AmtContactKill(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtRecentLiftRecord(&pCtx->RecentLifts, NowQpc, oldX, oldY);
            DbgPrint("[AmtPtp] UP solo id=%lu X=%u Y=%u qpc=%I64d\n",
                     oldId, oldX, oldY, NowQpc);
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
        DbgPrint("[AmtPtp] NewIdentity(origin==0) pool=%Iu oldId=%lu WasInGesture=%u qpc=%I64d\n",
                 p, pCtx->ActiveContacts[p].ContactID,
                 pCtx->ActiveContacts[p].WasInGesture, NowQpc);
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
            // BUG FIX (silent birth drop under palm/edge-rest pool
            // pressure): the pool is only MAX_CONTACTS (5) slots, and a
            // PALM_LOCAL-frozen contact (palmLocalFrozen[] above - e.g. a
            // wrist/palm edge resting near the bottom dead zone while
            // typing or gesturing) holds its slot ACTIVE indefinitely for
            // as long as it's physically touching, since Phase A freezes
            // it in place rather than killing it. With real fingers also
            // down, this can exhaust the pool - and this candidate,
            // representing a genuine new touch, used to just be dropped
            // here with a bare "continue": no DOWN ever reported, no
            // retry, nothing. Silent and invisible - exactly matching two
            // reported symptoms: a quick soft tap or the start of a
            // double-tap sometimes not registering at all, and the third
            // finger of a 3-finger swipe sometimes only being recognized
            // a frame or two late (Windows briefly sees 2 fingers, not 3,
            // until this candidate finally finds room).
            //
            // Reclaim a frozen slot instead of giving up: a PALM_LOCAL-
            // frozen contact already reports Confidence=0 (Phase A above)
            // and Windows already ignores it for pointer/gesture purposes
            // - so ending it early to make room for a REAL touch costs
            // nothing Windows was actually using. If the resting palm/
            // wrist is still there next frame, it simply re-qualifies as
            // PALM_LOCAL again and gets reborn frozen under a fresh
            // ContactID (Kill->Birth, the same sanctioned identity-churn
            // pattern used everywhere else in this file) - invisible to
            // Windows either way. Only reclaims a FROZEN slot - if all 5
            // are genuine, live, non-frozen touches, there is truly
            // nothing safe to free, and this candidate is still dropped
            // (unreachable in practice: that would mean 6 simultaneous
            // real contacts, beyond this hardware's own reporting limit).
            size_t reclaimIdx = MAX_CONTACTS;
            for (size_t rp = 0; rp < MAX_CONTACTS; rp++) {
                if (pCtx->ActiveContacts[rp].State == CONTACT_ACTIVE &&
                    palmLocalFrozen[rp]) {
                    reclaimIdx = rp;
                    break;
                }
            }

            if (reclaimIdx == MAX_CONTACTS) {
                continue;
            }

            ULONG  reclaimedId; USHORT reclaimedX, reclaimedY;
            AmtContactKill(pCtx->ActiveContacts, reclaimIdx,
                           &reclaimedId, &reclaimedX, &reclaimedY);
            AmtCoreEmitContact(pCtx, OutResult, reclaimedId, reclaimedX, reclaimedY,
                               CONTACT_PHASE_UP, TRUE);
            // Clear the stale flag: this index is about to be reborn as a
            // real, non-frozen touch below - if a LATER candidate in this
            // same Phase B loop also needs to reclaim a slot, it must not
            // mistake this freshly-birthed real contact for still being
            // the frozen palm/edge entry it used to be.
            palmLocalFrozen[reclaimIdx] = FALSE;
            freeIdx = reclaimIdx;
        }

        USHORT liftX, liftY;
        BOOLEAN looksLikeRetap =
            AmtRecentLiftFindNearby(&pCtx->RecentLifts, NowQpc,
                                    pCtx->PerfFrequency.QuadPart,
                                    cand->X, cand->Y, &liftX, &liftY);

        DbgPrint("[AmtPtp] BIRTH X=%u Y=%u looksLikeRetap=%u liftX=%u liftY=%u qpc=%I64d\n",
                 cand->X, cand->Y, looksLikeRetap, liftX, liftY, NowQpc);

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

        // BUG FIX (single-frame noise taint): don't latch WasInGesture off
        // one qualifying frame alone - see GESTURE_TAINT_DEBOUNCE_FRAMES.
        // Only touches contacts not already tainted; an already-tainted
        // contact keeps re-qualifying every gestureThisFrame frame same as
        // before (no behavior change for an established gesture).
        if (!pCtx->ActiveContacts[p].WasInGesture) {
            if (shouldTaint) {
                if (pCtx->ActiveContacts[p].GestureCandidateFrames < 255)
                    pCtx->ActiveContacts[p].GestureCandidateFrames++;
                if (pCtx->ActiveContacts[p].GestureCandidateFrames >=
                    GESTURE_TAINT_DEBOUNCE_FRAMES)
                {
                    pCtx->ActiveContacts[p].WasInGesture = TRUE;
                    DbgPrint("[AmtPtp] WasInGesture SET pool=%Iu id=%lu qpc=%I64d\n",
                             p, pCtx->ActiveContacts[p].ContactID, NowQpc);
                }
            } else {
                pCtx->ActiveContacts[p].GestureCandidateFrames = 0;
            }
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

        // DIAG (soft-tap/double-tap investigation): DOWN is the one phase
        // that was never logged anywhere - UP has two prints (solo/
        // gesture-tainted), BIRTH has one, but a tap's DOWN edge, its
        // Confident bit, and the WasInGesture state it starts with were
        // all invisible. Windows' own PTP recognizer decides tap vs
        // double-tap vs click purely from the Confident/ContactID/
        // ScanTime sequence we hand it - if a DOWN reports Confident=0,
        // or WasInGesture=1 (retap smoothing/tail-overlap taint), the
        // recognizer can silently drop that tap without anything else in
        // this driver seeing an error. Remove once the soft-double-tap
        // report is resolved.
        if (justBorn) {
            DbgPrint("[AmtPtp] DOWN id=%lu X=%u Y=%u Confident=%u WasInGesture=%u RetapSeeded=%u qpc=%I64d\n",
                     pCtx->ActiveContacts[p].ContactID, repX, repY, reportConfident,
                     pCtx->ActiveContacts[p].WasInGesture,
                     pCtx->ActiveContacts[p].RetapSeeded, NowQpc);
        }

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
    // state this frame.
    //
    // Force touch itself is never decided - or reported - while the
    // button is still held. It is resolved from two one-way latches
    // accumulated over the press:
    //   - ForceTouchDragLockout (set elsewhere above, once the finger
    //     passes FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE from the button-down
    //     anchor) - if this ever trips, the press is a hard-tap/drag and
    //     is committed as HARD_TAP immediately, the instant it trips,
    //     regardless of pressure;
    //   - ClickArbitrationPressureCrossed - latched TRUE the first frame
    //     framePeakPressure exceeds FORCE_TOUCH_PRESSURE_THRESHOLD.
    // While PressureCrossed is FALSE, reporting the ordinary click early
    // and retracting it later if pressure does end up crossing the
    // threshold would itself show up in Windows as a real, brief
    // left-click that shouldn't have happened, right before the
    // force-touch right-click - trading one glitch for a worse one. So
    // as long as there's still a chance this press crosses the
    // threshold, the click stays withheld.
    //
    // That "still a chance" window is bounded by
    // CLICK_ARBITRATION_GRACE_MS, not by pressure hysteresis or by
    // waiting for release: a genuine force touch ramps pressure past the
    // threshold within well under this window in practice, so once it
    // elapses with pressure still below threshold, this press is judged
    // decisively not a force touch - HARD_TAP commits right then and
    // there, live, same as an ordinary click would report. This is what
    // makes long, pressure-quiet holds (click-and-hold with no drag) work
    // normally again: they no longer sit unreported for the entire
    // duration of the hold, only for this one bounded grace window at
    // the very start. Once PressureCrossed goes TRUE, the grace timer is
    // irrelevant - that press is a force-touch candidate for the rest of
    // the hold and its outcome (FORCE_TOUCH, or HARD_TAP if it later
    // drags) is still only ever revealed at release, exactly as before.
    if (!ButtonDown) {
        if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_PENDING) {
            pCtx->ClickArbitrationState = pCtx->ClickArbitrationPressureCrossed
                ? CLICK_ARBITRATION_FORCE_TOUCH
                : CLICK_ARBITRATION_HARD_TAP;
        } else {
            pCtx->ClickArbitrationState = CLICK_ARBITRATION_IDLE;
        }
    } else {
        // BUG FIX: gate the fresh-press reset on buttonClickEdge (the
        // authoritative rising-edge signal computed above, tracked via
        // PrevButtonClicked every frame) rather than on
        // ClickArbitrationState still reading IDLE. The two are usually
        // the same thing, but not guaranteed: the IDLE reset above only
        // runs on a frame where ButtonDown is FALSE, so a release
        // immediately followed by a re-press with no intervening
        // not-down frame in between (two presses landing in the same, or
        // adjacent, USB interrupt completions) would leave a stale
        // FORCE_TOUCH or HARD_TAP state sitting in ClickArbitrationState
        // from the PREVIOUS press when this new one begins. Without this
        // fix, that stale state would either resume being treated as
        // still-decided (dropping the entire new press - no click, no
        // force touch, nothing reported for it) rather than starting a
        // fresh PENDING decision. buttonClickEdge doesn't have this gap:
        // it's derived from PrevButtonClicked, which is unconditionally
        // updated every single frame regardless of ButtonDown's value.
        if (buttonClickEdge) {
            pCtx->ClickArbitrationState           = CLICK_ARBITRATION_PENDING;
            pCtx->ClickArbitrationPressureCrossed = FALSE;
            pCtx->ClickArbitrationStartQpc        = NowQpc;
        }
        if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_PENDING) {
            if (framePeakPressure > FORCE_TOUCH_PRESSURE_THRESHOLD) {
                pCtx->ClickArbitrationPressureCrossed = TRUE;
            }
            if (pCtx->ForceTouchDragLockout) {
                // Movement wins, unconditionally, the instant it trips -
                // a press that's moving is a drag, full stop, regardless
                // of how hard it's pressed or whether it already crossed
                // the force-touch threshold earlier this same press.
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
            } else if (!pCtx->ClickArbitrationPressureCrossed) {
                LONGLONG elapsedTicks = NowQpc - pCtx->ClickArbitrationStartQpc;
                LONGLONG graceTicks = (pCtx->PerfFrequency.QuadPart > 0)
                    ? (pCtx->PerfFrequency.QuadPart * CLICK_ARBITRATION_GRACE_MS) / 1000
                    : 0; // no usable clock - fail open to a live click below
                if (elapsedTicks >= graceTicks) {
                    pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
                }
            }
        }
        // else: HARD_TAP already latched - hold it. (FORCE_TOUCH is never
        // latched while ButtonDown - see the release branch above - so
        // there is nothing to hold or downgrade here for that case.)
    }

    *OutButtonClickReport =
        (pCtx->ClickArbitrationState == CLICK_ARBITRATION_HARD_TAP);

    // Force-touch is a discrete pulse now, not a held state - and it is
    // emitted as a complete down+up PAIR on the exact same frame that
    // resolves a press to FORCE_TOUCH (release, see the branch above).
    //
    // BUG FIX: an earlier version of this spread the pulse across two
    // frames - DownEdge on the resolution frame, UpEdge deferred to
    // whatever PTPCore_ProcessFrame call happened to come next (via
    // ForceTouchActive latch/diff, the same pattern the old held-state
    // design used). That's fine while the pad is still active, but by
    // definition this resolution only happens at release, typically with
    // no finger on the pad and the button no longer down - there is no
    // guarantee another USB interrupt completion arrives promptly enough
    // (or at all, before the pad goes idle) to carry that deferred
    // UpEdge. Interrupt.c also drops a completion outright if no HID
    // read request happens to be queued at that moment
    // (WdfIoQueueRetrieveNextRequest failing at the top of the handler),
    // which is an extra way a lone "next frame" up-edge could simply
    // never arrive. Net effect: Button2 could get stuck reported as held
    // down in Windows until the pad happened to see more activity.
    // Emitting both edges together removes the dependency on any future
    // frame entirely - PendingForceTouchEdgeQueue in Interrupt.c already
    // enqueues DownEdge and UpEdge as independent booleans and was
    // already built to carry a down+up pair through to mouhid.sys in
    // order even when both fire close together, so this needs no changes
    // on that side. ForceTouchActive is unused by this path now (nothing
    // is ever "held") - kept FALSE at all times.
    BOOLEAN forceTouchPulse =
        (pCtx->ClickArbitrationState == CLICK_ARBITRATION_FORCE_TOUCH);

    *OutForceTouchDownEdge = forceTouchPulse;
    *OutForceTouchUpEdge   = forceTouchPulse;
    pCtx->ForceTouchActive = FALSE;
}