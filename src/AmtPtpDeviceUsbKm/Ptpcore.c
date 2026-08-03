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
// TUNING: raised from 70 to 250 (2026-07-31) - at this hardware's ~120-125
// raw units/mm (see LOGICAL_MAXIMUM comment in AppleDefinition.h vs the
// MacBook Pro 16" 2019's ~160x99mm physical pad), 70 units was ~0.55-0.6mm
// - tighter than typical finger micro-tremor during a deliberate firm
// press, so legitimate force-touch attempts were getting downgraded to
// HARD_TAP by their own tremor alone. 250 units is ~2mm: forgiving enough
// to absorb tremor/grip-shift during a press, still well short of a
// genuine intentional drag (window/file/selection).
// TUNING: lowered ~35% from 250 to 160 (2026-08-03) - real-device testing
// showed the opposite problem from the original 70-unit issue: 250 units
// (~2mm) was now too forgiving in the other direction. A deliberate
// window/file drag - press down hard, then immediately start moving to
// reposition - often stayed under 250 units of travel long enough that
// FORCE_TOUCH still latched first, so the drag had to travel noticeably
// further before ForceTouchDragLockout retroactively downgraded it to
// HARD_TAP. 160 units (~1.3mm) still comfortably clears ordinary finger
// tremor/grip-shift during a stationary press, but lets an intentional
// drag get reclassified much sooner.
#define FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE 160

// Absolute wall-clock safety-net cap: once the button goes down, no press
// stays PENDING longer than this no matter what, even one that keeps
// inching up without ever stalling for CLICK_ARBITRATION_STALL_FRAMES_
// FAST_RESOLVE in a row. The common case resolves much sooner, via either
// the stall fast-exit below (ordinary flat/light press) or the drop-from-
// peak hysteresis (a press that peaked and is now receding). Tunable.
// TUNING: raised from 60 to 90 (2026-07-31) - now that the stall fast-exit
// above resolves an ordinary press in ~3 frames (~24ms), this cap no
// longer gates typical click latency at all; it only ever matters for a
// press that keeps climbing slowly/unevenly toward the threshold without
// stalling. Widening it gives a deliberately slow (not sharp/sudden)
// force-touch press more room to actually reach the threshold, with no
// downside for the common case.
#define CLICK_ARBITRATION_TIMEOUT_MS 90

// A press is considered to be receding once its pressure falls this many
// raw units below the highest value seen so far this press (rather than
// below the immediately preceding frame - the sensor is noisy enough
// that a single frame-to-frame dip isn't reliable on its own). Tunable;
// ~3-5 units is enough margin to absorb sensor noise without meaningfully
// delaying the decision.
#define CLICK_ARBITRATION_PRESSURE_HYSTERESIS 4

// FIX (soft-press fast resolve): consecutive frames the running peak
// pressure must go without growing before an otherwise-undecided PENDING
// press is treated as an ordinary click, instead of waiting out the full
// CLICK_ARBITRATION_TIMEOUT_MS. Only a press that is still actively
// climbing toward FORCE_TOUCH_PRESSURE_THRESHOLD needs the long window -
// an ordinary press plateaus almost immediately and has no reason to hold
// up the click report. NOT calibrated against real hardware scan cadence
// (assumed ~120Hz/~8ms per frame elsewhere in this file, so 3 frames is
// ~24ms) - revisit if real-device testing shows force-touch presses
// naturally stalling for a frame or two mid-ramp (sensor noise, grip
// micro-adjustment) and getting cut off too early; raise toward 5-6 if so.
// TUNING: raised 3 -> 6 (2026-08-03) - real-device testing showed exactly
// this: an intermittent, press-to-press-dependent failure to register
// force touch (works, works, then randomly doesn't, no consistent
// pattern). A genuine firm press does not always ramp monotonically -
// a brief ~24-40ms plateau mid-ramp (muscle micro-pause while still
// bearing down harder) was tripping the OLD 3-frame fast-resolve and
// locking the press into HARD_TAP before pressure ever reached
// FORCE_TOUCH_PRESSURE_THRESHOLD. 6 frames (~48ms) gives a real ramp
// enough room to survive a short plateau while still resolving an
// ordinary flat/light click well under CLICK_ARBITRATION_TIMEOUT_MS.
#define CLICK_ARBITRATION_STALL_FRAMES_FAST_RESOLVE 6

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
    //
    // FIX (same root cause as reportConfident below): an Unconfirmed
    // candidate (Match.c - brand-new, below-tip-threshold, no pool
    // anchor, existence not yet verified) must not count as a "finger"
    // here either. Before this fix, a single-frame noise blob landing in
    // the same frame as a genuine solo tap's birth inflated aliveCount to
    // 2, which (a) set gestureThisFrame=TRUE and (b) fed the
    // otherCandidateUntainted/taintedCoAliveCount check further down,
    // together capable of falsely tainting an otherwise ordinary solo tap
    // as WasInGesture. A gesture-tainted lift deliberately skips
    // AmtRecentLiftRecord (see the Phase A comment below), so the next
    // fast tap right after it can't find a nearby recent lift and loses
    // its retap smoothing - a plausible contributor to "second tap of a
    // fast double-tap sometimes doesn't register" on top of the
    // Confidence-flicker fix above.
    UCHAR aliveCount = 0;
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        if (!candidates.Candidates[ci].PalmLocal &&
            !candidates.Candidates[ci].Unconfirmed)
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
        // FIX (3rd-finger-join taint loss): the rule above breaks the
        // opposite, equally-real case - a 3rd finger touching down onto
        // an ALREADY-ESTABLISHED, still fully-live 2-finger gesture, both
        // of whose fingers are tainted and still physically down. Here
        // EVERY co-alive partner is already-tainted, so the untainted-
        // partner rule alone (see below) would refuse to taint the join.
        // Disambiguated by COUNT, not just presence: a dying tail is
        // exactly one straggler; a still-active multi-finger gesture has
        // two-or-more participants concurrently down. See below.
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
            UCHAR   taintedCoAliveCount     = 0;
            for (UCHAR oc = 0; oc < candidates.Count; oc++) {
                if (oc == ci || candidates.Candidates[oc].PalmLocal) continue;
                if (matchResult.CorrespondingPoolIndex[oc] == MATCH_NO_CORRESPONDENCE)
                    continue;
                if (!candidateWasTainted[oc]) {
                    otherCandidateUntainted = TRUE;
                } else {
                    taintedCoAliveCount++;
                }
            }

            // FIX (3rd-finger-join taint loss): the tail-overlap check
            // above (otherCandidateUntainted) can't tell apart two
            // situations that look identical from "is there an untainted
            // co-alive partner": (a) an old gesture down to its LAST
            // finger, still matched-continuing this exact frame, next to
            // an unrelated new solo tap - the tail-overlap case the audit
            // fix above was written for, where the new birth must NOT be
            // tainted; and (b) a genuine 3rd finger touching down onto an
            // ALREADY-ESTABLISHED, still fully-live 2-finger gesture
            // (e.g. scroll), where both existing fingers are tainted and
            // still physically down - here the new birth SHOULD be
            // tainted, or the whole contact drops out of the gesture and
            // gets processed as a stray solo contact once it lifts.
            //
            // The distinguishing signal is the COUNT of already-tainted
            // co-alive partners, not just their existence: a dying tail
            // has exactly one straggler left; a still-active multi-finger
            // gesture has two or more participants concurrently down.
            // Two-or-more tainted co-alive partners can only mean the
            // latter, so force-taint the birth in that case regardless
            // of whether some OTHER untainted candidate also happens to
            // be present this frame.
            shouldTaint = otherCandidateUntainted || (taintedCoAliveCount >= 2);
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

        // AUDIT FIX (click Confidence false-negative) + FIX (soft-tap
        // Confidence flicker): Confidence must track whether this
        // contact's EXISTENCE is established, not whether its position
        // this exact frame is stale/bridged (TipDropApplied) or whether
        // it happens to be holding still (isStationary, Match.c) - a
        // deliberate soft tap or a held mechanical click both sit nearly
        // motionless by nature, and penalizing that was exactly backwards:
        // it used to report Confident=TRUE only on a contact's single
        // ambiguous first frame (unverified birth) and Confident=FALSE on
        // every steady frame after, for as long as it stayed still - the
        // worst possible ordering for a short-lived tap, and the direct
        // cause of soft 2-finger taps intermittently reading as 1 or 3
        // fingers to Windows' PTP stack, and solo soft taps intermittently
        // not registering at all. See MATCH_CANDIDATE.Unconfirmed
        // (Match.h/Match.c) - TRUE only for a genuinely brand-new,
        // below-tip-threshold, no-pool-anchor candidate (can't yet rule
        // out sensor noise); FALSE for everything else, including an
        // anchored bridge candidate whether moving or stationary, and
        // including a buttonClickEdge rebind (always an already-tracked,
        // real finger by construction - identity swap only, per
        // AmtContactRebindIdentity).
        BOOLEAN reportConfident = rebindThisFrame[p]
            ? TRUE
            : (BOOLEAN)(cand->Unconfirmed == 0);

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
    //
    // FIX (fast-click loss): a physical press+release shorter than
    // whatever PENDING needed to resolve (threshold cross, hysteresis
    // drop, stall fast-exit, or the CLICK_ARBITRATION_TIMEOUT_MS safety
    // net) used to vanish entirely - ButtonDown going FALSE force-reset
    // ClickArbitrationState straight to IDLE below, so *OutButtonClickReport
    // (computed from ClickArbitrationState after this block) read FALSE on
    // every single frame of that press: never PENDING->HARD_TAP because
    // release cut it off first, never TRUE on the release frame itself
    // because IDLE isn't HARD_TAP either. A press that ends while still
    // PENDING never got the chance to reach FORCE_TOUCH_PRESSURE_THRESHOLD,
    // so it is by definition not a force touch - report it as an ordinary
    // click on this exact release frame via releasedFastClick below,
    // instead of silently dropping it. FORCE_TOUCH/HARD_TAP presses don't
    // need this: they already reported themselves TRUE on an earlier held
    // frame before release ever arrived.
    BOOLEAN releasedFastClick = FALSE;

    if (!ButtonDown) {
        if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_PENDING) {
            releasedFastClick = TRUE;
        }
        pCtx->ClickArbitrationState = CLICK_ARBITRATION_IDLE;
    } else {
        if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_IDLE) {
            pCtx->ClickArbitrationState         = CLICK_ARBITRATION_PENDING;
            pCtx->ClickArbitrationStartQpc      = NowQpc;
            pCtx->ClickArbitrationPeakPressure  = framePeakPressure;
            pCtx->ClickArbitrationStallFrames   = 0;
        }

        if (pCtx->ForceTouchDragLockout) {
            // Movement past the lockout distance always wins, this frame
            // or any later one - including retroactively cancelling a
            // FORCE_TOUCH decision already made earlier this same press.
            pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
        } else if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_PENDING) {
            if (framePeakPressure > pCtx->ClickArbitrationPeakPressure) {
                pCtx->ClickArbitrationPeakPressure = framePeakPressure;
                // Still climbing toward the threshold this frame - this is
                // exactly the case that genuinely needs measuring, so keep
                // resetting the stall counter for as long as it keeps
                // growing. Comparison against the running PEAK (not the
                // previous frame's raw sample) makes this noise-robust the
                // same way the drop-from-peak hysteresis below already is:
                // the peak only ever moves up, so per-frame sensor jitter
                // below it can't spuriously "reset progress".
                pCtx->ClickArbitrationStallFrames = 0;
            } else if (pCtx->ClickArbitrationStallFrames < 255) {
                pCtx->ClickArbitrationStallFrames++;
            }

            INT dropFromPeak = (INT)pCtx->ClickArbitrationPeakPressure -
                                (INT)framePeakPressure;

            if (framePeakPressure > FORCE_TOUCH_PRESSURE_THRESHOLD) {
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_FORCE_TOUCH;
            } else if (dropFromPeak >= CLICK_ARBITRATION_PRESSURE_HYSTERESIS) {
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
            } else if (pCtx->ClickArbitrationStallFrames >= CLICK_ARBITRATION_STALL_FRAMES_FAST_RESOLVE) {
                // FIX (soft-press fast resolve): an ordinary flat/light
                // press neither climbs toward the threshold nor recedes
                // enough to trip the hysteresis exit above - peak just
                // sits at whatever the steady pressure is, so it used to
                // sit PENDING for the full CLICK_ARBITRATION_TIMEOUT_MS
                // every single time, no matter how obviously it was never
                // a force-touch attempt. Only a press that is actively
                // still climbing needs the full window measured out; one
                // that has stopped growing for
                // CLICK_ARBITRATION_STALL_FRAMES_FAST_RESOLVE consecutive
                // frames has already shown its hand and can resolve now.
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
            } else {
                // Safety net: still climbing (or too new to tell) - fall
                // back to the absolute wall-clock cap so a slow, genuine
                // force-touch ramp that never quite stalls for
                // STALL_FRAMES_FAST_RESOLVE in a row (e.g. it inches up by
                // 1 unit every other frame) still can't wait forever.
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
        releasedFastClick ||
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