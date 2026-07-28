// Match.c - PTPCore ContactMatcher.

#include "Driver.h"
#include "Match.h"
#include "Palm.h"

// Max per-frame finger movement. Shared with tip-drop anchor search.
#define MATCH_MAX_CONTINUATION_DELTA 4000

// Must match MATCH_MAX_CONTINUATION_DELTA to prevent false Confidence=1
// on fast-but-light drags (soft-drift confidence bug fix).
#define TIP_DROP_MAX_REPOSITION_DELTA MATCH_MAX_CONTINUATION_DELTA

// Slot-hint used only to break near-ties, never fixed-cost subtraction.
#define MATCH_TIE_EPSILON_SQ 4  // ~2 units linear distance, squared

// Stationary deadzone for debounce bridge. Real coords if moving.
#define TIP_DROP_STATIONARY_DELTA       3

static UCHAR
AmtMatchCandidateTip(_In_ USHORT major, _In_ USHORT minor)
{
    return (UCHAR)(((INT)major << 1) >= 200 || ((INT)minor << 1) >= 150);
}

// Third-tier matching tie-break (see AmtMatchCorrespond): when two
// candidate/pool pairs are tied on both spatial cost (within
// MATCH_TIE_EPSILON_SQ) and slot-hint match, prefer whichever pairing
// keeps touch geometry/pressure most similar to what that pool entry
// last reported. This is a plain Manhattan-style sum, not a weighted/
// normalized model - Major/Minor/Pressure are different raw units, but
// for a last-resort tie-break among candidates that are already
// spatially near-identical, "closer on all three, unweighted" is a
// reasonable cheap heuristic and avoids inventing per-axis weights with
// no real-device data to justify them.
static LONG
AmtMatchShapeDistance(
    _In_ const MATCH_CANDIDATE* Cand,
    _In_ const ACTIVE_CONTACT*  Contact
)
{
    INT dMajor = (INT)Cand->Major - (INT)Contact->LastMajor;
    if (dMajor < 0) dMajor = -dMajor;
    INT dMinor = (INT)Cand->Minor - (INT)Contact->LastMinor;
    if (dMinor < 0) dMinor = -dMinor;
    INT dPressure = (INT)Cand->Pressure - (INT)Contact->LastPressure;
    if (dPressure < 0) dPressure = -dPressure;

    return (LONG)dMajor + dMinor + dPressure;
}

VOID
AmtMatchBuildCandidates(
    _In_  const RAW_FRAME*                        RawFrame,
    _In_  const struct BCM5974_CONFIG*             DevInfo,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _Out_ MATCH_CANDIDATE_SET*                     OutCandidates,
    _Out_ BOOLEAN*                                 LargePalmDetected
)
{
    *LargePalmDetected = FALSE;
    RtlZeroMemory(OutCandidates, sizeof(MATCH_CANDIDATE_SET));

    for (UCHAR i = 0; i < RawFrame->ContactCount; i++) {
        const RAW_CONTACT* rc = &RawFrame->Contacts[i];

        PALM_CLASS palm = AmtPalmClassify(rc->Major, rc->Minor, DevInfo,
                                          (INT)rc->X, (INT)rc->Y);

        if (palm == PALM_LARGE) {
            *LargePalmDetected = TRUE;
            OutCandidates->Count = 0; // blank whole pad
            return;
        }

        MATCH_CANDIDATE cand;
        RtlZeroMemory(&cand, sizeof(cand));
        cand.SlotIndex     = rc->SlotIndex;
        cand.IdentityBreak = (rc->Origin == 0);
        cand.Major         = rc->Major;
        cand.Minor         = rc->Minor;
        cand.Pressure      = rc->Pressure;

        if (palm == PALM_LOCAL) {
            cand.PalmLocal = TRUE;
            cand.X = rc->X;
            cand.Y = rc->Y;
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
            continue;
        }

        if (AmtMatchCandidateTip(rc->Major, rc->Minor)) {
            cand.X = rc->X;
            cand.Y = rc->Y;
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
            continue;
        }

        // Below tip threshold: scan all pool entries by LastSlotHint,
        // keep nearest within TIP_DROP_MAX_REPOSITION_DELTA.
        size_t bestPoolIdx  = MAX_CONTACTS;
        LONG   bestDistSq   = -1;

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (Pool[p].State != CONTACT_ACTIVE)
                continue;
            if (Pool[p].LastSlotHint != rc->SlotIndex)
                continue;

            INT dxAbs = (INT)rc->X - (INT)Pool[p].ReportX;
            if (dxAbs < 0) dxAbs = -dxAbs;
            INT dyAbs = (INT)rc->Y - (INT)Pool[p].ReportY;
            if (dyAbs < 0) dyAbs = -dyAbs;

            if (dxAbs > TIP_DROP_MAX_REPOSITION_DELTA ||
                dyAbs > TIP_DROP_MAX_REPOSITION_DELTA)
                continue;

            LONG distSq = (LONG)dxAbs * dxAbs + (LONG)dyAbs * dyAbs;
            if (bestPoolIdx == MAX_CONTACTS || distSq < bestDistSq) {
                bestPoolIdx = p;
                bestDistSq  = distSq;
            }
        }

        if (bestPoolIdx == MAX_CONTACTS) {
            // No anchor: full-confidence birth candidate.
            cand.X              = rc->X;
            cand.Y              = rc->Y;
            cand.TipDropApplied = 0;
            OutCandidates->Candidates[OutCandidates->Count++] = cand;
            continue;
        }

        // Bridge candidate through; real coords if moving, anchor if stationary.
        INT dxMove = (INT)rc->X - (INT)Pool[bestPoolIdx].ReportX;
        INT dyMove = (INT)rc->Y - (INT)Pool[bestPoolIdx].ReportY;
        if (dxMove < 0) dxMove = -dxMove;
        if (dyMove < 0) dyMove = -dyMove;

        BOOLEAN isStationary = (dxMove <= TIP_DROP_STATIONARY_DELTA) &&
                               (dyMove <= TIP_DROP_STATIONARY_DELTA);

        cand.X = isStationary ? Pool[bestPoolIdx].ReportX : rc->X;
        cand.Y = isStationary ? Pool[bestPoolIdx].ReportY : rc->Y;

        // AUDIT FIX: this was previously hardcoded to 0 in both branches,
        // silently disabling the documented "Confidence=FALSE on stale
        // bridged position" contract (see Match.h / Interrupt.c's
        // outC->Confident = (cand->TipDropApplied == 0)). Only the
        // stationary/anchor branch actually reports a stale position
        // (Pool[bestPoolIdx].ReportX/Y instead of the live rc->X/Y), so
        // only that branch should mark low confidence. The moving branch
        // reports the real live coordinate and stays full-confidence.
        cand.TipDropApplied = isStationary ? 1 : 0;

        OutCandidates->Candidates[OutCandidates->Count++] = cand;
    }
}

VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  PerfFrequencyHz,
    _Out_ MATCH_RESULT*                              OutResult
)
{
    RtlZeroMemory(OutResult, sizeof(MATCH_RESULT));

    BOOLEAN poolClaimed[MAX_CONTACTS];
    RtlZeroMemory(poolClaimed, sizeof(poolClaimed));

    // Greedy minimum-cost assignment (N,M <= 5).
    typedef struct { UCHAR candIdx; size_t poolIdx; LONG cost; BOOLEAN slotHintMatch; } PAIR;
    PAIR pairs[PTP_MAX_CONTACT_POINTS * MAX_CONTACTS];
    UCHAR pairCount = 0;

    for (UCHAR ci = 0; ci < Candidates->Count; ci++) {
        const MATCH_CANDIDATE* cand = &Candidates->Candidates[ci];

        OutResult->CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;

        if (cand->PalmLocal)
            continue;

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (Pool[p].State != CONTACT_ACTIVE)
                continue;

            INT dx = (INT)cand->X - (INT)Pool[p].ReportX;
            INT dy = (INT)cand->Y - (INT)Pool[p].ReportY;
            LONG dist = (LONG)dx * dx + (LONG)dy * dy; // squared distance

            pairs[pairCount].candIdx       = ci;
            pairs[pairCount].poolIdx       = p;
            pairs[pairCount].cost          = dist;
            pairs[pairCount].slotHintMatch = (cand->SlotIndex == Pool[p].LastSlotHint);
            pairCount++;
        }
    }

    // Greedy matching by ascending cost. Slot-hint tie-breaker within epsilon.
    BOOLEAN pairUsed[PTP_MAX_CONTACT_POINTS * MAX_CONTACTS];
    RtlZeroMemory(pairUsed, sizeof(pairUsed));

    BOOLEAN candClaimed[PTP_MAX_CONTACT_POINTS];
    RtlZeroMemory(candClaimed, sizeof(candClaimed));

    for (UCHAR pick = 0; pick < pairCount; pick++) {
        LONG    bestCost          = -1;
        UCHAR   bestIdx           = 0;
        BOOLEAN bestSlotHintMatch = FALSE;
        LONG    bestShapeDist     = 0;
        BOOLEAN found             = FALSE;

        for (UCHAR k = 0; k < pairCount; k++) {
            if (pairUsed[k]) continue;
            if (candClaimed[pairs[k].candIdx]) continue;
            if (poolClaimed[pairs[k].poolIdx]) continue;

            if (!found) {
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = pairs[k].slotHintMatch;
                bestShapeDist     = AmtMatchShapeDistance(
                    &Candidates->Candidates[pairs[k].candIdx], &Pool[pairs[k].poolIdx]);
                found             = TRUE;
                continue;
            }

            LONG delta = pairs[k].cost - bestCost;
            BOOLEAN withinEpsilon = (delta > -MATCH_TIE_EPSILON_SQ) &&
                                    (delta < MATCH_TIE_EPSILON_SQ);

            if (pairs[k].cost < bestCost && !withinEpsilon) {
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = pairs[k].slotHintMatch;
                bestShapeDist     = AmtMatchShapeDistance(
                    &Candidates->Candidates[pairs[k].candIdx], &Pool[pairs[k].poolIdx]);
            } else if (withinEpsilon && pairs[k].slotHintMatch && !bestSlotHintMatch) {
                bestCost          = pairs[k].cost;
                bestIdx           = k;
                bestSlotHintMatch = TRUE;
                bestShapeDist     = AmtMatchShapeDistance(
                    &Candidates->Candidates[pairs[k].candIdx], &Pool[pairs[k].poolIdx]);
            } else if (withinEpsilon && pairs[k].slotHintMatch == bestSlotHintMatch) {
                // Cost and slot-hint both tied - fall through to shape.
                LONG kShapeDist = AmtMatchShapeDistance(
                    &Candidates->Candidates[pairs[k].candIdx], &Pool[pairs[k].poolIdx]);
                if (kShapeDist < bestShapeDist) {
                    bestCost      = pairs[k].cost;
                    bestIdx       = k;
                    bestShapeDist = kShapeDist;
                    // bestSlotHintMatch unchanged - already equal to pairs[k]'s.
                }
            }
        }

        if (!found)
            break;

        pairUsed[bestIdx]                 = TRUE;
        UCHAR  ci = pairs[bestIdx].candIdx;
        size_t p  = pairs[bestIdx].poolIdx;

        // Reject implausible matches.
        BOOLEAN spatialReject = pairs[bestIdx].cost >
            (LONG)MATCH_MAX_CONTINUATION_DELTA * MATCH_MAX_CONTINUATION_DELTA;

        // Time-domain rejection. LastSeenQpc=0 -> never updated, skip time check.
        BOOLEAN timeReject = FALSE;
        if (Pool[p].LastSeenQpc != 0 && PerfFrequencyHz > 0) {
            LONGLONG deltaTicks = NowQpc - Pool[p].LastSeenQpc;
            LONGLONG maxTicks   = (MATCH_MAX_TIME_DELTA_100NS * PerfFrequencyHz) / 10000000LL;
            timeReject = (NowQpc < Pool[p].LastSeenQpc) || (deltaTicks > maxTicks);
        }

        if (spatialReject || timeReject) {
            candClaimed[ci] = TRUE;
            continue;
        }

        candClaimed[ci] = TRUE;
        poolClaimed[p]  = TRUE;

        OutResult->CorrespondingPoolIndex[ci] = p;
        OutResult->NewIdentity[ci] =
            Candidates->Candidates[ci].IdentityBreak ? TRUE : FALSE;
    }

    // Unclaimed -> lift.
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        if (Pool[p].State != CONTACT_ACTIVE)
            continue;
        if (!poolClaimed[p]) {
            OutResult->UnmatchedPoolIndices[OutResult->UnmatchedCount++] = p;
        }
    }
}
