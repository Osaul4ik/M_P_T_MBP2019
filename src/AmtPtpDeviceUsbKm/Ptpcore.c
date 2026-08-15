// PTPCore.c - Frame orchestration: Phase A (lift) -> Phase B (birth) -> Phase C (update).

#include "Driver.h"
#include "PTPCore.h"
#include "ActiveContact.h"
#include "Match.h"

// Dragging far from the anchor cancels force-touch arbitration.
#define FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE 160

// Safety cap prevents a press from staying pending indefinitely.
// CLICK_ARBITRATION_TIMEOUT_MS moved to Ptpcore.h (shared with Device.c).

// Hysteresis avoids false resolution on tiny pressure dips.
#define CLICK_ARBITRATION_PRESSURE_HYSTERESIS 4

// Fast resolve avoids delaying ordinary taps while still allowing firm presses.
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
    _In_  LONGLONG                WindowTicks,
    _In_  USHORT                  CandX,
    _In_  USHORT                  CandY,
    _Out_ USHORT*                 OutX,
    _Out_ USHORT*                 OutY
)
{
    if (WindowTicks <= 0) {
        *OutX = 0;
        *OutY = 0;
        return FALSE;
    }

    ULONGLONG bestDistSq = ~0ULL;
    BOOLEAN   found      = FALSE;
    USHORT   bestX = 0, bestY = 0;

    for (UCHAR i = 0; i < RECENT_LIFT_CAPACITY; i++) {
        const RECENT_LIFT* e = &Ring->Entries[i];
        if (!e->Valid)
            continue;
        if (NowQpc < e->LiftQpc)
            continue; // QPC must be monotonic
        if (NowQpc - e->LiftQpc > WindowTicks)
            continue;

        INT dx = AmtAbsDelta((INT)CandX, (INT)e->X);
        INT dy = AmtAbsDelta((INT)CandY, (INT)e->Y);

        if (dx > RETAP_MAX_DISTANCE || dy > RETAP_MAX_DISTANCE)
            continue;

        ULONGLONG distSq = AmtDistSq(dx, dy);
        if (!found || distSq < bestDistSq) {
            bestDistSq = distSq;
            bestX      = e->X;
            bestY      = e->Y;
            found      = TRUE;
        }
    }

    // Always written (even when !found, in which case these are just the
    // zero-initialized defaults above) so OutX/OutY satisfy their _Out_
    // contract on every path. Callers gate on the return value before
    // reading these, so this doesn't change any observed behavior.
    *OutX = bestX;
    *OutY = bestY;
    return found;
}

// Fill one committed PTP_CORE_CONTACT slot and advance ContactCount.
// Identical fill logic previously duplicated between AmtCoreEmitContact
// (fresh reports) and AmtCoreDrainOverflow (deferred reports) - both only
// differ in where the field values come from and what they do after.
static __inline VOID
AmtCoreWriteContactSlot(
    _Inout_ PTP_CORE_FRAME* OutResult,
    _In_    ULONG           ContactID,
    _In_    USHORT          X,
    _In_    USHORT          Y,
    _In_    CONTACT_PHASE   Phase,
    _In_    BOOLEAN         Confident
)
{
    PPTP_CORE_CONTACT outC = &OutResult->Contacts[OutResult->ContactCount];
    outC->ContactID   = ContactID;
    outC->X           = X;
    outC->Y           = Y;
    outC->Phase       = Phase;
    outC->Confident   = Confident;
    outC->PalmSuspect = FALSE;
    OutResult->ContactCount++;
}

// Emit a contact report and queue overflow entries when needed.
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
        AmtCoreWriteContactSlot(OutResult, ContactID, X, Y, Phase, Confident);
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
    // else: overflow queue full too - event dropped. Unreachable with
    // PTP_MAX_CONTACT_POINTS physical fingers (worst case = all UPs + all
    // DOWNs, which exactly fits one frame report + one full queue).
}

// Avoid emitting duplicate contact IDs in one frame.
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
    // Compacted write cursor keeps the queue dense; keep <= k, so
    // writing Overflow*[keep] before reading Overflow*[k] is safe.
    UCHAR keep = 0;

    for (UCHAR k = 0; k < pCtx->OverflowCount; k++) {
        ULONG id = pCtx->OverflowContactID[k];

        if (AmtPoolHoldsContactID(pCtx->ActiveContacts, id)) {
            // Still live - a fresh report comes this frame; drop the stale
            // queued one to avoid duplicating the ID.
            continue;
        }

        if (OutResult->ContactCount < PTP_MAX_CONTACT_POINTS) {
            AmtCoreWriteContactSlot(OutResult, pCtx->OverflowContactID[k],
                                    pCtx->OverflowX[k], pCtx->OverflowY[k],
                                    pCtx->OverflowPhase[k], pCtx->OverflowConfident[k]);
            continue;
        }

        // Orphaned but still no room this frame - keep queued.
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
    _Out_   BOOLEAN*         OutForceTouchClick,
    _Out_   BOOLEAN*         OutButtonClickReport
)
{
    PDEVICE_CONTEXT pCtx = DeviceContext;

    // Click-edge (rising 0->1) for the button-rebirth workaround (Phase
    // A.5): Windows' anti-jitter would otherwise "snap" the click to the
    // cursor if the delta looks like jitter. A forced rebind routes it
    // through the non-snapping soft-tap TipSwitch path instead.
    BOOLEAN buttonClickEdge = ButtonDown && !pCtx->PrevButtonClicked;
    pCtx->PrevButtonClicked = ButtonDown;

    // OPTIMIZATION: was RtlZeroMemory(OutResult, sizeof(PTP_CORE_FRAME)) -
    // zeroing all PTP_MAX_CONTACT_POINTS Contacts[] slots (~80 of the
    // struct's ~96 bytes) every single frame. Unnecessary: every consumer
    // (AmtSerializeCoreFrameToReport, and this file) only ever reads
    // Contacts[0..ContactCount-1], and every slot that gets counted is
    // fully field-initialized by AmtCoreEmitContact/AmtCoreDrainOverflow
    // before ContactCount is incremented past it - nothing ever reads a
    // stale/uninitialized tail slot. Only the 3 scalar fields actually
    // need a defined starting value.
    OutResult->TimestampQpc     = NowQpc;
    OutResult->ContactCount     = 0;
    OutResult->LargePalmBlanked = FALSE;

    // Build candidates (palm + tip-debounce).
    // Small-contact rejection follows the same admission model as
    // RequirePressureToActivate on Force-Touch devices: it is checked only
    // when a brand-new contact is about to be born. Once a candidate has
    // correspondence with an existing active contact, it is allowed through
    // regardless of later size changes. This avoids a separate raw-frame
    // latch and relies on the existing contact-pool lifecycle.

    MATCH_CANDIDATE_SET candidates;
    BOOLEAN              largePalm = FALSE;

    AmtMatchBuildCandidates(RawFrame, pCtx->DeviceInfo, &pCtx->PalmConfig,
                            &pCtx->PalmRuntime, pCtx->ActiveContacts, &candidates, &largePalm);

    // Palm session: suppress candidates when palm active. Contacts that go
    // unmatched purely because of this suppression are NOT lifted (see
    // Phase A below) - they're frozen at low confidence instead, so no
    // phantom UP/tap fires while the hand is still on the pad.
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
                       NowQpc, pCtx->MatchMaxTimeDeltaTicks,
                       &matchResult);

    // FIX: an Unconfirmed candidate (new, below-tip-threshold, no pool
    // anchor) must not count as a finger - a single-frame noise blob could
    // falsely taint a solo tap as WasInGesture and break retap smoothing.
    //
    // BUG FIX (phantom UP from bottom/edge cutoff): PALM_LOCAL candidates
    // are never matched to pool entries, so a tracked contact dragging into
    // the edge dead zone looked like a lift. Freeze those pool slots
    // (matched via LastSlotHint) in Phase A instead of killing them.
    //
    // Merged into one candidates.Count pass - aliveCount and
    // palmLocalFrozen are independent per-candidate computations that
    // were previously two separate full scans of the same array.
    UCHAR   aliveCount = 0;
    BOOLEAN palmLocalFrozen[MAX_CONTACTS] = { 0 };
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];

        BOOLEAN isNewContact =
            (matchResult.CorrespondingPoolIndex[ci] == MATCH_NO_CORRESPONDENCE);

        BOOLEAN pressureGatedBirth =
            isNewContact &&
            pCtx->SupportsForceTouch &&
            pCtx->PointerConfig.ForceTouchEnabled &&
            pCtx->PointerConfig.RequirePressureToActivate &&
            cand->Pressure == 0;

        BOOLEAN smallContactGatedBirth =
            isNewContact &&
            !pCtx->SupportsForceTouch &&
            pCtx->PointerConfig.SmallContactRejectionEnabled &&
            cand->Major < 100 &&
            cand->Minor < 80;

        if (!pressureGatedBirth &&
            !smallContactGatedBirth &&
            !cand->PalmLocal &&
            !cand->Unconfirmed)
            aliveCount++;

        if (!cand->PalmLocal)
            continue;

        USHORT slotIndex = cand->SlotIndex;
        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (pCtx->ActiveContacts[p].State == CONTACT_ACTIVE &&
                pCtx->ActiveContacts[p].LastSlotHint == slotIndex)
            {
                palmLocalFrozen[p] = TRUE;
                break;
            }
        }
    }

    // Per-frame taint gate: >=2 fingers down this frame.
    BOOLEAN gestureThisFrame = (aliveCount >= 2);

    // Drain deferred lift-offs
    AmtCoreDrainOverflow(pCtx, OutResult);

    // Phase A (lift): unmatched pool entries lift.
    // Gesture-tainted: defer kill on last finger; solo: kill immediately.

    for (UCHAR u = 0; u < matchResult.UnmatchedCount; u++) {
        size_t p = matchResult.UnmatchedPoolIndices[u];
        PACTIVE_CONTACT ac = &pCtx->ActiveContacts[p];

        if (palmSuppressedFrame || palmLocalFrozen[p]) {
            // Still physically down but classified as palm or in the edge
            // dead zone. Do NOT kill it (Windows reads a fast DOWN->UP as a
            // tap/drag-end) - freeze it: same ContactID, last-known
            // position, TipSwitch=1, Confidence=0. Real UP only when the
            // raw frame reports zero contacts.
            //
            // AUDIT FIX: skipping AmtContactUpdate also skips refreshing
            // LastSeenQpc. Frozen beyond MATCH_MAX_TIME_DELTA_100NS, the
            // stale timestamp would make AmtMatchCorrespond time-reject the
            // correct spatial match on re-qualification - killing and
            // re-birthing a finger that never left the pad. Refresh
            // LastSeenQpc directly.
            ac->LastSeenQpc = NowQpc;

            AmtCoreEmitContact(pCtx, OutResult, ac->ContactID,
                               ac->ReportX, ac->ReportY,
                               CONTACT_PHASE_MOVE, FALSE);
            continue;
        }

        ULONG  oldId; USHORT oldX, oldY;

        if (ac->WasInGesture) {
            // Gesture-tainted: lift immediately. The previous fake-MOVE-then-UP
            // deferral delayed the real UP by up to 4 frames (~33ms @ 120Hz),
            // breaking 3-finger swipe completion and 2-finger soft taps.
            // Not recorded in RecentLifts.
            AmtContactEnterGrace(pCtx->ActiveContacts, p, &oldId, &oldX, &oldY);
            AmtContactExpireGrace(pCtx->ActiveContacts, p);
            AmtCoreEmitContact(pCtx, OutResult, oldId, oldX, oldY, CONTACT_PHASE_UP, TRUE);

        } else {
            // Solo contact: kill immediately. palmSuppressedFrame is always
            // FALSE here - handled above, never falls through.
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

    // Phase A.5 (button-click forced rebirth): on the RISING edge of the
    // integrated button, force a new ContactID onto every still-live,
    // pre-existing matched contact, in place, at its own position. Routes
    // the click through the non-snapping soft-tap TipSwitch path. The pool
    // slot is NOT freed/reacquired - WasInGesture/FramesAlive/HystX/Y/
    // ReportX/Y carry across since the finger never left the pad.
    // Pre-existing == LastSeenQpc != 0; contacts born THIS frame are skipped.
    BOOLEAN rebindThisFrame[MAX_CONTACTS] = { 0 };

    if (buttonClickEdge) {
        for (UCHAR ci = 0; ci < candidates.Count; ci++) {
            if (candidates.Candidates[ci].PalmLocal) continue;

            size_t p = matchResult.CorrespondingPoolIndex[ci];
            if (p == MATCH_NO_CORRESPONDENCE) continue;
            PACTIVE_CONTACT ac = &pCtx->ActiveContacts[p];
            if (ac->LastSeenQpc == 0) continue; // born this frame

            USHORT oldX = ac->ReportX;
            USHORT oldY = ac->ReportY;

            ULONG oldId;
            AmtContactRebindIdentity(pCtx->ActiveContacts, p, &pCtx->NextContactId, &oldId);
            AmtCoreEmitContact(pCtx, OutResult, oldId, oldX, oldY, CONTACT_PHASE_UP, TRUE);
            // Deliberately NOT AmtRecentLiftRecord'd - not a real lift and
            // must not seed retap-smoothing for unrelated future taps.

            rebindThisFrame[p] = TRUE;

            // AUDIT FIX: rebind doesn't touch LastSeenQpc, but Phase C
            // decides DOWN-vs-MOVE purely from (LastSeenQpc == 0). Without
            // forcing it to 0, the new ContactID would report MOVE instead
            // of DOWN, defeating the non-snapping soft-tap path.
            // AmtContactUpdate overwrites it with real NowQpc shortly after.
            ac->LastSeenQpc = 0;
        }
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    // Phase B (birth): unmatched candidates birth new pool entries.
    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];
        if (cand->PalmLocal) continue;
        if (matchResult.CorrespondingPoolIndex[ci] != MATCH_NO_CORRESPONDENCE)
            continue; // handled in Phase C

        // Admission gates apply ONLY when creating a brand-new contact.
        // Once a candidate has corresponded to an existing active contact,
        // Phase C updates/reports it without re-applying these gates.
        //
        // Force-Touch devices: require positive pressure when configured.
        // Non-Force-Touch devices: require Major >= 100 OR Minor >= 80 when
        // Small Contact Rejection is enabled.
        if (pCtx->SupportsForceTouch) {
            if (pCtx->PointerConfig.ForceTouchEnabled &&
                pCtx->PointerConfig.RequirePressureToActivate &&
                cand->Pressure == 0)
            {
                continue;
            }
        } else if (pCtx->PointerConfig.SmallContactRejectionEnabled &&
                   cand->Major < 100 &&
                   cand->Minor < 80)
        {
            continue;
        }

        size_t freeIdx = AmtContactPoolFindFree(pCtx->ActiveContacts);
        if (freeIdx == MAX_CONTACTS) {
            continue;
        }

        USHORT liftX, liftY;
        BOOLEAN looksLikeRetap =
            AmtRecentLiftFindNearby(&pCtx->RecentLifts, NowQpc,
                                    pCtx->RetapWindowTicks,
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

        // NOTE: WasInGesture is NOT decided here - Phase C makes the final
        // taint decision; setting it here is just overwritten.

        matchResult.CorrespondingPoolIndex[ci] = freeIdx;
    }

    // Phase C (update / report): update once, report once.
    //
    // Pre-pass: snapshot fresh births (LastSeenQpc==0) and WasInGesture
    // BEFORE the mutating loop, so the tail-overlap taint check sees a
    // stable, order-independent picture.
    //
    // AUDIT FIX: a button-click rebind also has LastSeenQpc==0 (forced in
    // Phase A.5) but is NOT a true birth - routing it through the "inherit
    // taint from an untainted partner" logic could drop WasInGesture from a
    // finger mid-gesture. candidateIsTrueBirth separates an actual Phase B
    // allocation from a rebind; justBorn (DOWN vs MOVE) stays on the plain
    // LastSeenQpc test.
    BOOLEAN candidateJustBorn[PTP_MAX_CONTACT_POINTS]    = { 0 };
    BOOLEAN candidateWasTainted[PTP_MAX_CONTACT_POINTS]  = { 0 };
    BOOLEAN candidateIsTrueBirth[PTP_MAX_CONTACT_POINTS] = { 0 };
    for (UCHAR pc = 0; pc < candidates.Count; pc++) {
        if (candidates.Candidates[pc].PalmLocal) continue;
        size_t pp = matchResult.CorrespondingPoolIndex[pc];
        if (pp == MATCH_NO_CORRESPONDENCE) continue;
        const ACTIVE_CONTACT* acPre = &pCtx->ActiveContacts[pp];
        candidateJustBorn[pc]    = (acPre->LastSeenQpc == 0);
        candidateWasTainted[pc]  = acPre->WasInGesture;
        candidateIsTrueBirth[pc] = candidateJustBorn[pc] && !rebindThisFrame[pp];
    }

    for (UCHAR ci = 0; ci < candidates.Count; ci++) {
        const MATCH_CANDIDATE* cand = &candidates.Candidates[ci];
        if (cand->PalmLocal) continue;

        size_t p = matchResult.CorrespondingPoolIndex[ci];
        if (p == MATCH_NO_CORRESPONDENCE) continue;
        PACTIVE_CONTACT c = &pCtx->ActiveContacts[p];

        BOOLEAN justBorn = candidateJustBorn[ci];

        // AUDIT FIX (tail-overlap false taint): aliveCount>=2 this frame
        // does NOT always mean a co-starting gesture - a dying gesture's last
        // finger can overlap a brand-new solo tap for one transient frame.
        // Tainting the new birth routed it through the gesture-lift path,
        // silently eating taps right after a gesture release.
        //
        // Only TRUE pool births (not rebinds) inherit taint conditionally:
        // only if ANOTHER live candidate was NOT already tainted at frame
        // start - the signature of a genuine gesture start. A co-alive
        // already-tainted partner is the tail of a separate gesture and must
        // NOT taint the new birth.
        //
        // FIX (3rd-finger-join): a 3rd finger joining an established
        // 2-finger gesture has EVERY partner already tainted. Disambiguate
        // by COUNT: a dying tail has exactly one straggler; an active
        // gesture has >=2 tainted partners concurrently down.
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

            // >=2 already-tainted co-alive partners can only mean an
            // established multi-finger gesture (a dying tail has just one
            // straggler), so force-taint the birth.
            shouldTaint = otherCandidateUntainted || (taintedCoAliveCount >= 2);
        }

        if (shouldTaint) {
            c->WasInGesture = TRUE;
        }

        USHORT repX, repY;
        AmtContactUpdate(c, cand->X, cand->Y,
                         cand->Major, cand->Minor, cand->Pressure,
                         cand->SlotIndex, NowQpc, pCtx->PerfFrequency.QuadPart,
                         (BOOLEAN)(aliveCount == 1), gestureThisFrame,
                         &pCtx->PointerConfig, &pCtx->ScrollConfig,
                         &pCtx->PointerRuntime, &pCtx->ScrollRuntime,
                         &repX, &repY);

        // ARCHITECTURE FIX (PTP spec alignment): per the Windows Precision
        // Touchpad spec, Confidence means exactly one thing - "this is an
        // intentional finger, not noise/palm." That question is already
        // answered upstream by AmtPalmClassify (Palm.c): every candidate
        // that reaches this loop has PalmLocal==FALSE (palm-classified
        // candidates are filtered out above) and PALM_LARGE already blanked
        // the whole frame earlier in AmtMatchBuildCandidates. So by this
        // point palm/noise rejection is already done - Confidence is always
        // TRUE here.
        //
        // Unconfirmed (Match.c) answers a DIFFERENT question - "is contact
        // area large enough to trust raw X/Y for matching/bridging" - a
        // geometry-quality signal for the matcher, not an intentionality
        // signal. A light-but-deliberate soft tap has small major/minor
        // (low Unconfirmed confidence) while being just as intentional as a
        // firm press; gating Confidence on it made Windows discard genuine
        // light taps as noise on their first (sometimes only) frame.
        // Unconfirmed still gates aliveCount (gesture-taint) - that usage
        // is unaffected.
        BOOLEAN reportConfident = TRUE;

        AmtCoreEmitContact(pCtx, OutResult, c->ContactID,
                           repX, repY,
                           justBorn ? CONTACT_PHASE_DOWN : CONTACT_PHASE_MOVE,
                           reportConfident);
    }

    AmtContactPoolCheckInvariants(pCtx->ActiveContacts);

    if (!pCtx->SupportsForceTouch || !pCtx->PointerConfig.ForceTouchEnabled) {
        pCtx->ForceTouchAnchorValid = FALSE;
        pCtx->ForceTouchDragLockout = FALSE;
        pCtx->ClickArbitrationState = ButtonDown
            ? CLICK_ARBITRATION_HARD_TAP
            : CLICK_ARBITRATION_IDLE;

        *OutButtonClickReport = ButtonDown;
        *OutForceTouchClick   = FALSE;
        return;
    }

    // Force-touch drag lockout + peak pressure. Both used to scan
    // RawFrame->Contacts in two separate loops, gated on the same
    // ButtonDown condition and independent of each other - merged into
    // one pass. Arming the anchor stays its own step BEFORE the pass
    // (unchanged order/timing: a same-frame arm is still immediately
    // eligible for the distance search below, exactly as before).
    USHORT framePeakPressure = 0;

    if (!ButtonDown) {
        pCtx->ForceTouchAnchorValid = FALSE;
        pCtx->ForceTouchDragLockout = FALSE;
    } else {
        // AUDIT FIX: previously gated on buttonClickEdge, the anchor was
        // never armed if the edge frame reported zero contacts (mechanical
        // click flex can do that) - so drags could never be reclassified.
        // Arm on !ForceTouchAnchorValid instead: first frame with contacts
        // after button-down, once per press.
        if (!pCtx->ForceTouchAnchorValid && RawFrame->ContactCount > 0) {
            pCtx->ForceTouchAnchorX     = RawFrame->Contacts[0].X;
            pCtx->ForceTouchAnchorY     = RawFrame->Contacts[0].Y;
            pCtx->ForceTouchAnchorValid = TRUE;
        }

        // BUG FIX: compare only the ONE contact nearest the anchor.
        // Contacts are in sensor scan order, not stable slots - testing
        // all of them let an unrelated second finger trip the lockout.
        BOOLEAN trackAnchor = pCtx->ForceTouchAnchorValid;
        ULONGLONG bestDistSq = ~0ULL;
        INT     bestDx = 0, bestDy = 0;

        for (UCHAR fi = 0; fi < RawFrame->ContactCount; fi++) {
            // Peak raw pressure this frame, shared by click arbitration
            // and force-touch below. Use the RAW frame (same click-flex
            // rationale).
            if (RawFrame->Contacts[fi].Pressure > framePeakPressure)
                framePeakPressure = RawFrame->Contacts[fi].Pressure;

            if (!trackAnchor)
                continue;

            INT dx = (INT)RawFrame->Contacts[fi].X - (INT)pCtx->ForceTouchAnchorX;
            INT dy = (INT)RawFrame->Contacts[fi].Y - (INT)pCtx->ForceTouchAnchorY;
            ULONGLONG distSq = AmtDistSq(dx, dy);
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                bestDx     = dx;
                bestDy     = dy;
            }
        }

        if (trackAnchor && bestDistSq != ~0ULL) {
            INT adx = (bestDx < 0) ? -bestDx : bestDx;
            INT ady = (bestDy < 0) ? -bestDy : bestDy;
            if (adx > FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE ||
                ady > FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE) {
                pCtx->ForceTouchDragLockout = TRUE;
            }
        }
    }

    // Click arbitration: force-touch vs ordinary hard-tap. PENDING resolves
    // to FORCE_TOUCH on pressure threshold cross (if no drag lockout), to
    // HARD_TAP on a meaningful drop from peak, on stall fast-exit, or on the
    // CLICK_ARBITRATION_TIMEOUT_MS safety net.
    //
    // REWORK: drag lockout is checked EVERY frame and applies even to a
    // latched FORCE_TOUCH - move past the lockout distance at any point and
    // the press is unconditionally HARD_TAP (one-way downgrade).
    //
    // FIX (fast-click loss): press+release shorter than PENDING's resolution
    // used to vanish entirely. A press ending while still PENDING never
    // reached the threshold, so it's not a force touch - report it as an
    // ordinary click on the release frame via releasedFastClick.
    BOOLEAN releasedFastClick = FALSE;

    // Captured BEFORE the !ButtonDown branch below resets
    // ClickArbitrationState to IDLE - see the force-touch delivery block
    // near the end for why this is needed.
    BOOLEAN releaseWasForceTouch =
        !ButtonDown && (pCtx->ClickArbitrationState == CLICK_ARBITRATION_FORCE_TOUCH);

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
            // Movement past the lockout always wins, this frame or any later
            // one - including retroactively cancelling a FORCE_TOUCH already
            // decided earlier this same press.
            pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
        } else if (pCtx->ClickArbitrationState == CLICK_ARBITRATION_PENDING) {
            if (framePeakPressure > pCtx->ClickArbitrationPeakPressure) {
                pCtx->ClickArbitrationPeakPressure = framePeakPressure;
                // Still climbing - keep resetting the stall counter. Peak
                // comparison is noise-robust (only moves up).
                pCtx->ClickArbitrationStallFrames = 0;
            } else if (pCtx->ClickArbitrationStallFrames < 255) {
                pCtx->ClickArbitrationStallFrames++;
            }

            INT dropFromPeak = (INT)pCtx->ClickArbitrationPeakPressure -
                                (INT)framePeakPressure;

            if (framePeakPressure > pCtx->PointerConfig.ForceTapThreshold) {
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_FORCE_TOUCH;
            } else if (dropFromPeak >= CLICK_ARBITRATION_PRESSURE_HYSTERESIS) {
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
            } else if (pCtx->ClickArbitrationStallFrames >= CLICK_ARBITRATION_STALL_FRAMES_FAST_RESOLVE) {
                // FIX (soft-press fast resolve): a flat/light press that has
                // stopped growing for STALL_FRAMES_FAST_RESOLVE consecutive
                // frames is never a force touch - resolve now instead of
                // waiting out the full timeout.
                pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
            } else {
                // Safety net: absolute wall-clock cap so a slow ramp that
                // never stalls in a row still can't wait forever.
                LONGLONG elapsedTicks = NowQpc - pCtx->ClickArbitrationStartQpc;
                LONGLONG timeoutTicks = pCtx->ClickArbitrationTimeoutTicks; // MICRO-OPT: cached at D0Entry
                if (elapsedTicks >= timeoutTicks) {
                    pCtx->ClickArbitrationState = CLICK_ARBITRATION_HARD_TAP;
                }
            }
        }
        // else: FORCE_TOUCH already latched and drag lockout hasn't tripped
        // (checked above) - hold it. HARD_TAP is likewise held.
    }

    *OutButtonClickReport =
        releasedFastClick ||
        (pCtx->ClickArbitrationState == CLICK_ARBITRATION_HARD_TAP);

    // Force-touch delivery: REWORKED (2026-08-03) to fire only on release.
    // The old design sent the right-click DOWN edge as soon as FORCE_TOUCH
    // latched, relying on drag lockout to send UP later - but a fast
    // press-and-drag could deliver a complete Button2 pair before the
    // downgrade caught up, flashing the context menu open and closed. Fix:
    // don't act mid-press. Only a press STILL FORCE_TOUCH at the exact
    // moment of release fires the context menu, as a single down+up pulse
    // on that release frame. Trades away "right-click-drag", which Windows
    // has no use for.
    //
    // SIMPLIFICATION (2026-08-03): down and up are always reported together
    // (both TRUE on the release frame) - collapsed to a single
    // OutForceTouchClick output.
    *OutForceTouchClick = releaseWasForceTouch;
}