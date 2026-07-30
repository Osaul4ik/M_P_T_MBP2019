// Match.c - PTPCore ContactMatcher.

#include "Driver.h"
#include "Match.h"
#include "Palm.h"

// Max per-frame finger movement. Shared with tip-drop anchor search.
#define MATCH_MAX_CONTINUATION_DELTA 4000

// BUG FIX (spurious origin==0 mid-touch identity churn - see
// AmtMatchCorrespond below): how far a firmware-flagged "identity break"
// candidate may land from its matched pool entry's last reported
// position and still be honored as a genuine new touch. 600 mirrors
// RETAP_MAX_DISTANCE (Activecontact.h) - the same "still basically the
// same spot" scale already used elsewhere for a deliberate fast re-tap.
// Anything beyond this is not a plausible same-finger re-tap distance;
// see the AUDIT FIX comment at the call site for why that means it's
// firmware noise, not a second touch.
#define IDENTITY_BREAK_MAX_PLAUSIBLE_JUMP 600

// Must match MATCH_MAX_CONTINUATION_DELTA to prevent false Confidence=1
// on fast-but-light drags (soft-drift confidence bug fix).
#define TIP_DROP_MAX_REPOSITION_DELTA MATCH_MAX_CONTINUATION_DELTA

// Stationary deadzone for debounce bridge. Real coords if moving.
#define TIP_DROP_STATIONARY_DELTA       3

// Squared distance in LONGLONG. A plain 32-bit (LONG)dx * dx is fine for
// the un-predicted last-report distance below (real device coordinates
// stay well inside a range where that never overflows), but the
// PREDICTED position (AmtMatchPredictPosition) is clamped to ReportX/Y's
// full USHORT range and can legitimately reach its edges under fast
// motion plus a near-max allowed staleness gap (MATCH_MAX_TIME_DELTA_100NS)
// - at which point a 32-bit squared distance can overflow. Using
// LONGLONG for both keeps them on the same type (MATCH_EDGE.cost) without
// needing a separate special case for which one is "safe".
static LONGLONG
AmtMatchSquaredDist(_In_ INT Dx, _In_ INT Dy)
{
    return (LONGLONG)Dx * Dx + (LONGLONG)Dy * Dy;
}

static UCHAR
AmtMatchCandidateTip(_In_ USHORT major, _In_ USHORT minor)
{
    return (UCHAR)(((INT)major << 1) >= 200 || ((INT)minor << 1) >= 150);
}

// Third-tier matching tie-break (see AmtMatchCorrespond): when two
// candidate/pool pairs are tied on both spatial cost and slot-hint
// match, prefer whichever pairing keeps touch geometry/pressure most
// similar to what that pool entry last reported.
//
// NORMALIZED against DevInfo's own calibrated ranges (previously a raw,
// unweighted ADC-unit sum - see git history). Major/Minor/Pressure are
// still different raw units with different native ranges (e.g. width
// 0..2048 vs pressure 0..300 on a typical panel, per BCM5974_CONFIG), so
// summing them directly let whichever axis happens to have the larger
// raw range dominate the tie-break regardless of how much it actually
// changed, proportionally. Scaling each axis's delta by
// MATCH_SHAPE_NORM_SCALE/range first (AmtMatchNormalizeDelta) puts all
// three on the same "percent of that axis's calibrated span" footing
// before summing - still a plain sum, not a hand-tuned weighted model,
// but now an apples-to-apples one. Width range is shared by Major and
// Minor (DevInfo->w) since the hardware reports both on the same raw
// scale; pressure uses DevInfo->p.
#define MATCH_SHAPE_NORM_SCALE 256

static LONG
AmtMatchNormalizeDelta(_In_ INT delta, _In_ INT range)
{
    if (delta < 0) delta = -delta;
    if (range <= 0) range = 1; // guard: degenerate/missing calibration range
    return ((LONG)delta * MATCH_SHAPE_NORM_SCALE) / range;
}

static LONG
AmtMatchShapeDistance(
    _In_ const MATCH_CANDIDATE* Cand,
    _In_ const ACTIVE_CONTACT*  Contact,
    _In_ INT                    WidthRange,    // DevInfo->w.max - w.min, caller-hoisted
    _In_ INT                    PressureRange  // DevInfo->p.max - p.min, caller-hoisted
)
{
    INT dMajor    = (INT)Cand->Major    - (INT)Contact->LastMajor;
    INT dMinor    = (INT)Cand->Minor    - (INT)Contact->LastMinor;
    INT dPressure = (INT)Cand->Pressure - (INT)Contact->LastPressure;

    return AmtMatchNormalizeDelta(dMajor, WidthRange)
         + AmtMatchNormalizeDelta(dMinor, WidthRange)
         + AmtMatchNormalizeDelta(dPressure, PressureRange);
}

// Clamp a coordinate-space value back into ReportX/Y's USHORT range
// before truncating - same clamp-before-truncate pattern as
// AmtContactCommitSample's gesture-scroll branch in ActiveContact.c.
// Guards AmtMatchPredictPosition against wraparound if a runaway
// velocity value ever extrapolates outside the representable range.
static USHORT
AmtMatchClampCoord(_In_ LONGLONG Value)
{
    if (Value < 0)      return 0;
    if (Value > 0xFFFF) return 0xFFFF;
    return (USHORT)Value;
}

// Dead-reckoned prediction: where this pool entry's contact is expected
// to be RIGHT NOW, extrapolating its last known per-axis velocity
// (ACTIVE_CONTACT.VelocityX/Y - see AmtContactUpdate in ActiveContact.c)
// forward by the elapsed time since it was last updated. Falls back
// exactly to the last reported position whenever there's no reliable
// velocity/timestamp to extrapolate from (LastSeenQpc==0,
// PerfFrequencyHz<=0, or - the ordinary case for a stationary or
// freshly-born contact - VelocityX/Y==0), since the extrapolated delta
// is then 0. Used ONLY to rank/tie-break matching cost in
// AmtMatchCorrespond - never for the spatial/time feasibility gate,
// which stays anchored to the actual last report position, so a noisy
// single-frame velocity estimate can never widen what's ACCEPTED as a
// plausible continuation, only how already-accepted candidates rank.
static VOID
AmtMatchPredictPosition(
    _In_  const ACTIVE_CONTACT* Contact,
    _In_  LONGLONG              NowQpc,
    _In_  LONGLONG              PerfFrequencyHz,
    _Out_ USHORT*                PredX,
    _Out_ USHORT*                PredY
)
{
    LONGLONG deltaX = 0, deltaY = 0;

    if (Contact->LastSeenQpc != 0 && PerfFrequencyHz > 0 &&
        NowQpc > Contact->LastSeenQpc)
    {
        LONGLONG dtTicks = NowQpc - Contact->LastSeenQpc;
        // velocity (units/sec) * dt (ticks) / freq (ticks/sec) = units.
        // Multiply before divide for integer precision, same convention
        // as AmtContactClassifyVelocity in ActiveContact.c.
        deltaX = ((LONGLONG)Contact->VelocityX * dtTicks) / PerfFrequencyHz;
        deltaY = ((LONGLONG)Contact->VelocityY * dtTicks) / PerfFrequencyHz;
    }

    *PredX = AmtMatchClampCoord((LONGLONG)Contact->ReportX + deltaX);
    *PredY = AmtMatchClampCoord((LONGLONG)Contact->ReportY + deltaY);
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

        // TEMP DIAG (DebugView): every raw contact's firmware Origin and
        // the IdentityBreak decision derived from it. Remove once the
        // tap/gesture misfire repro is captured.
        DbgPrint("[AmtPtp] cand slot=%u origin=%u X=%u Y=%u IdentityBreak=%u\n",
                 rc->SlotIndex, rc->Origin, rc->X, rc->Y, cand.IdentityBreak);

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

// Per (candidate,pool) edge, precomputed once before the search.
typedef struct {
    BOOLEAN  feasible;       // pool active, in-window spatially and in time
    LONGLONG cost;           // squared distance, primary key - see AmtMatchSquaredDist
    BOOLEAN  slotHintMatch;  // secondary key
    LONG     shapeDist;      // tertiary key (see AmtMatchShapeDistance)
} MATCH_EDGE;

// Totals for one full candidate->pool assignment (a leaf of the search).
// Compared lexicographically: more matches beats fewer regardless of cost
// (a live continuation should never lose to leaving a candidate unmatched
// just because "unmatched" is cheaper - unmatched means lift+rebirth, which
// is the expensive outcome from the user's perspective); then lower total
// squared distance; then more slot-hint agreement; then lower total shape
// distance. Unlike the old per-pair epsilon compare, this is a real total
// order - no intransitive "A ties B, B ties C, A doesn't tie C" artifacts,
// and no dependence on which pair happened to be visited first.
typedef struct {
    UCHAR    matchedCount;
    LONGLONG totalCost;
    UCHAR slotHintMatches;
    LONG  totalShapeDist;
} MATCH_TOTALS;

static BOOLEAN
AmtMatchTotalsBetter(_In_ const MATCH_TOTALS* a, _In_ const MATCH_TOTALS* b)
{
    if (a->matchedCount     != b->matchedCount)     return a->matchedCount     > b->matchedCount;
    if (a->totalCost        != b->totalCost)        return a->totalCost        < b->totalCost;
    if (a->slotHintMatches  != b->slotHintMatches)  return a->slotHintMatches  > b->slotHintMatches;
    return a->totalShapeDist < b->totalShapeDist;
}

// Exact max-cardinality, min-cost bipartite assignment via bounded
// backtracking. Candidates and pool are both capped at PTP_MAX_CONTACT_POINTS
// (5 on real hardware), so this explores at most (MAX_CONTACTS+1)^Count
// leaves (<= 6^5 = 7776) - a handful of microseconds, and a fixed, provably
// terminating bound (no recursion depth beyond Candidates->Count, no
// allocation). That trivial cost is what buys correctness: unlike the old
// greedy pass, this always finds the assignment that is actually best by
// the rules above, so a jitter-sized cost difference between two crossing
// candidates can no longer make the wrong one "grab" a pool slot first and
// starve the correct match.
//
// BRANCH-AND-BOUND: once a first leaf is found, every subsequent call
// prunes if this partial assignment provably cannot produce a better
// leaf than BestTotals - see the two checks below. The bound on
// remaining matches is deliberately loose (CandCount - Ci, ignoring
// pool-claimed/feasibility overlap among the remaining candidates) so it
// stays correct - cheap to compute - as a safe upper bound rather than
// an exact one: pruning must never discard a subtree that could still
// win. Doesn't change the result (still the exact same total order as
// an unpruned search), only how many of the <=7776 leaves get visited.
static VOID
AmtMatchSearch(
    _In_    const MATCH_EDGE (*Edges)[MAX_CONTACTS],
    _In_    UCHAR             CandCount,
    _In_    UCHAR              Ci,
    _Inout_ BOOLEAN*          PoolClaimed,
    _Inout_ size_t*           Assignment,      // working assignment, per candidate
    _Inout_ MATCH_TOTALS*     RunningTotals,
    _Inout_ size_t*           BestAssignment,
    _Inout_ MATCH_TOTALS*     BestTotals,
    _Inout_ BOOLEAN*          BestFound
)
{
    if (Ci == CandCount) {
        if (!*BestFound || AmtMatchTotalsBetter(RunningTotals, BestTotals)) {
            *BestFound = TRUE;
            *BestTotals = *RunningTotals;
            RtlCopyMemory(BestAssignment, Assignment, CandCount * sizeof(size_t));
        }
        return;
    }

    if (*BestFound) {
        // Safe (possibly loose) upper bound on matchedCount reachable
        // from here: every remaining candidate matches something.
        UCHAR maxPossibleMatched =
            (UCHAR)(RunningTotals->matchedCount + (CandCount - Ci));

        if (maxPossibleMatched < BestTotals->matchedCount) {
            // Every completion from here scores lower on the primary
            // (matchedCount) key alone - can never catch up.
            return;
        }
        if (maxPossibleMatched == BestTotals->matchedCount &&
            RunningTotals->totalCost > BestTotals->totalCost)
        {
            // Best case this subtree can tie the primary key, but costs
            // only accumulate (every Edge cost is >= 0, never
            // subtracted before a leaf), so totalCost can only grow from
            // here - already behind BestTotals on the secondary key too,
            // so no completion of this branch can beat it.
            return;
        }
    }

    // Branch: leave this candidate unmatched (always a valid option).
    Assignment[Ci] = MATCH_NO_CORRESPONDENCE;
    AmtMatchSearch(Edges, CandCount, Ci + 1, PoolClaimed, Assignment,
                   RunningTotals, BestAssignment, BestTotals, BestFound);

    // Branch: try every still-free, feasible pool entry for this candidate.
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        if (PoolClaimed[p] || !Edges[Ci][p].feasible)
            continue;

        PoolClaimed[p] = TRUE;
        Assignment[Ci] = p;

        RunningTotals->matchedCount++;
        RunningTotals->totalCost        += Edges[Ci][p].cost;
        RunningTotals->slotHintMatches  += Edges[Ci][p].slotHintMatch ? 1 : 0;
        RunningTotals->totalShapeDist   += Edges[Ci][p].shapeDist;

        AmtMatchSearch(Edges, CandCount, Ci + 1, PoolClaimed, Assignment,
                       RunningTotals, BestAssignment, BestTotals, BestFound);

        RunningTotals->matchedCount--;
        RunningTotals->totalCost        -= Edges[Ci][p].cost;
        RunningTotals->slotHintMatches  -= Edges[Ci][p].slotHintMatch ? 1 : 0;
        RunningTotals->totalShapeDist   -= Edges[Ci][p].shapeDist;

        PoolClaimed[p] = FALSE;
    }
}

VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  const struct BCM5974_CONFIG*               DevInfo,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  PerfFrequencyHz,
    _Out_ MATCH_RESULT*                              OutResult
)
{
    RtlZeroMemory(OutResult, sizeof(MATCH_RESULT));

    for (UCHAR ci = 0; ci < Candidates->Count; ci++) {
        OutResult->CorrespondingPoolIndex[ci] = MATCH_NO_CORRESPONDENCE;
    }

    // Precompute the dead-reckoned predicted position ONCE per pool entry,
    // outside the (candidate, pool) double loop below. AmtMatchPredictPosition
    // depends only on `p` (the pool entry), never on which candidate `ci`
    // is being considered against it - computing it inside that loop would
    // redo the exact same 64-bit divide for every candidate that happens
    // to be a spatial fit for the same fast-moving slot (up to
    // Candidates->Count times over for the busiest one, vs. once here).
    // Skipped entirely for a stationary/fresh contact (Velocity==0, the
    // common case): the result would just be ReportX/Y unchanged anyway,
    // so this avoids two wasted divides per pool entry in that case too.
    USHORT predictedX[MAX_CONTACTS];
    USHORT predictedY[MAX_CONTACTS];
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        if (Pool[p].State != CONTACT_ACTIVE)
            continue;

        if (Pool[p].VelocityX == 0 && Pool[p].VelocityY == 0) {
            predictedX[p] = Pool[p].ReportX;
            predictedY[p] = Pool[p].ReportY;
        } else {
            AmtMatchPredictPosition(&Pool[p], NowQpc, PerfFrequencyHz,
                                    &predictedX[p], &predictedY[p]);
        }
    }

    // Precompute DevInfo's calibrated ranges ONCE - these only depend on
    // DevInfo, which is constant for this whole call, but
    // AmtMatchShapeDistance used to recompute the same two subtractions
    // on every (candidate, pool) pair (up to Candidates->Count *
    // MAX_CONTACTS times).
    INT widthRange    = DevInfo->w.max - DevInfo->w.min; // Major/Minor share this scale
    INT pressureRange = DevInfo->p.max - DevInfo->p.min;

    // Build the feasibility/cost edge for every (candidate, pool) pair up
    // front. PalmLocal candidates never participate - their row is left
    // all-infeasible so the search skips them, same as the old code's
    // "continue" before it ever built a pair for them.
    MATCH_EDGE edges[PTP_MAX_CONTACT_POINTS][MAX_CONTACTS];
    RtlZeroMemory(edges, sizeof(edges));

    for (UCHAR ci = 0; ci < Candidates->Count; ci++) {
        const MATCH_CANDIDATE* cand = &Candidates->Candidates[ci];
        if (cand->PalmLocal)
            continue;

        for (size_t p = 0; p < MAX_CONTACTS; p++) {
            if (Pool[p].State != CONTACT_ACTIVE)
                continue;

            // Feasibility gate stays anchored to the ACTUAL last report
            // position, unchanged - dead-reckoned prediction (below) only
            // ever affects ranking among pairs that already pass this.
            INT dx = (INT)cand->X - (INT)Pool[p].ReportX;
            INT dy = (INT)cand->Y - (INT)Pool[p].ReportY;
            LONGLONG dist = AmtMatchSquaredDist(dx, dy);

            // Spatial rejection - implausible jump, not a real continuation.
            BOOLEAN spatialReject = dist >
                (LONGLONG)MATCH_MAX_CONTINUATION_DELTA * MATCH_MAX_CONTINUATION_DELTA;

            // Time-domain rejection. LastSeenQpc=0 -> never updated, skip check.
            BOOLEAN timeReject = FALSE;
            if (Pool[p].LastSeenQpc != 0 && PerfFrequencyHz > 0) {
                LONGLONG deltaTicks = NowQpc - Pool[p].LastSeenQpc;
                LONGLONG maxTicks   = (MATCH_MAX_TIME_DELTA_100NS * PerfFrequencyHz) / 10000000LL;
                timeReject = (NowQpc < Pool[p].LastSeenQpc) || (deltaTicks > maxTicks);
            }

            if (spatialReject || timeReject)
                continue; // edge stays feasible=FALSE (RtlZeroMemory default)

            // Ranking cost: distance to the dead-reckoned PREDICTED
            // position (precomputed above), not the stale last-report
            // position. Collapses to the same `dist` computed above
            // whenever velocity is 0/unknown (stationary or fresh
            // contact), so this only changes anything for a contact that
            // was actually moving at a steady rate. NOT gated by
            // spatialReject above - a fast, legitimately-moving contact
            // can predict well past MATCH_MAX_CONTINUATION_DELTA from its
            // last report, which is exactly the case this ranking cost
            // exists to get right - only the LONGLONG width
            // (AmtMatchSquaredDist) needs to keep up with that.
            INT pdx = (INT)cand->X - (INT)predictedX[p];
            INT pdy = (INT)cand->Y - (INT)predictedY[p];
            LONGLONG predictedCost = AmtMatchSquaredDist(pdx, pdy);

            edges[ci][p].feasible      = TRUE;
            edges[ci][p].cost          = predictedCost;
            edges[ci][p].slotHintMatch = (cand->SlotIndex == Pool[p].LastSlotHint);
            edges[ci][p].shapeDist     = AmtMatchShapeDistance(cand, &Pool[p], widthRange, pressureRange);
        }
    }

    // Exact search over the whole assignment - see AmtMatchSearch.
    size_t assignment[PTP_MAX_CONTACT_POINTS];
    size_t bestAssignment[PTP_MAX_CONTACT_POINTS];
    for (UCHAR ci = 0; ci < Candidates->Count; ci++) {
        assignment[ci] = MATCH_NO_CORRESPONDENCE;
    }

    BOOLEAN      poolClaimed[MAX_CONTACTS];
    RtlZeroMemory(poolClaimed, sizeof(poolClaimed));

    MATCH_TOTALS runningTotals;
    RtlZeroMemory(&runningTotals, sizeof(runningTotals));
    MATCH_TOTALS bestTotals;
    RtlZeroMemory(&bestTotals, sizeof(bestTotals));
    BOOLEAN bestFound = FALSE;

    if (Candidates->Count > 0) {
        AmtMatchSearch(edges, Candidates->Count, 0, poolClaimed, assignment,
                       &runningTotals, bestAssignment, &bestTotals, &bestFound);
    }

    BOOLEAN poolClaimedFinal[MAX_CONTACTS];
    RtlZeroMemory(poolClaimedFinal, sizeof(poolClaimedFinal));

    if (bestFound) {
        for (UCHAR ci = 0; ci < Candidates->Count; ci++) {
            if (bestAssignment[ci] == MATCH_NO_CORRESPONDENCE)
                continue;

            size_t p = bestAssignment[ci];
            poolClaimedFinal[p] = TRUE;

            OutResult->CorrespondingPoolIndex[ci] = p;

            // AUDIT FIX (spurious origin==0 mid-touch identity churn):
            // firmware's Origin byte reports 0 ("identity break") in two
            // very different real situations that look identical here -
            // (a) a genuine fast lift+re-tap landing back near the same
            // spot, which Ptpcore.c's NewIdentity(origin==0) path exists
            // to turn into a clean UP+DOWN pair for Windows' tap/
            // double-tap recognizer (the intended case), and (b) a T2/
            // BCM5974 internal slot-reassignment glitch - confirmed via
            // DebugView repro (SAKURAMBPRO.log): every multi-finger
            // transition (2nd/3rd/4th finger joining or leaving) can make
            // the controller re-tag an ALREADY-ACTIVE, never-lifted
            // finger's slot with origin==0 for exactly one frame, paired
            // with a garbage position 1000-2500+ raw units from where
            // that same finger was reporting one frame earlier - nowhere
            // near a real finger's per-frame travel distance. The old
            // code trusted the flag unconditionally: forced a kill of the
            // correctly-tracked contact and a rebirth at that garbage
            // position, mid-gesture. Each rebirth is a brand-new
            // ContactID at a teleported position, which resets whatever
            // velocity/momentum Windows' PTP stack was accumulating for
            // that finger - repeated throughout a 2-finger scroll, this
            // is consistent with the reported "very slow scroll, no
            // inertia" (momentum never has a clean, continuous trajectory
            // to compute from) and, during quick re-taps, with "double
            // soft tap doesn't work" (the second tap's REAL finger
            // position gets discarded in favor of the glitch position).
            //
            // Fix: only honor IdentityBreak as authoritative when the
            // candidate's position is within IDENTITY_BREAK_MAX_PLAUSIBLE_
            // JUMP of the matched pool entry's last reported position -
            // i.e. a real "still basically the same spot" re-tap. Beyond
            // that distance, this candidate already won the spatial
            // match (bestAssignment) on its own merits - the match search
            // above only pairs candidates within MATCH_MAX_CONTINUATION_
            // DELTA (4000) of the pool entry's last position AND inside
            // MATCH_MAX_TIME_DELTA_100NS of its last-seen time in the
            // first place - so this is not "accept an implausible jump
            // instead of rejecting it," it's "stop treating an
            // already-accepted continuation as a brand-new identity just
            // because of a firmware flag with no reasoning documented for
            // why it should override spatial continuity."
            if (Candidates->Candidates[ci].IdentityBreak) {
#if AMT_RAW_DISABLE_IDENTITY_BREAK_FIX
                // RAW MODE: trust the firmware's IdentityBreak flag
                // unconditionally, exactly like before the plausible-
                // jump fix below existed - no suppression at all.
                OutResult->NewIdentity[ci] = TRUE;
#else
                INT ibDx = (INT)Candidates->Candidates[ci].X - (INT)Pool[p].ReportX;
                INT ibDy = (INT)Candidates->Candidates[ci].Y - (INT)Pool[p].ReportY;
                if (ibDx < 0) ibDx = -ibDx;
                if (ibDy < 0) ibDy = -ibDy;

                OutResult->NewIdentity[ci] =
                    (ibDx <= IDENTITY_BREAK_MAX_PLAUSIBLE_JUMP &&
                     ibDy <= IDENTITY_BREAK_MAX_PLAUSIBLE_JUMP);

                if (!OutResult->NewIdentity[ci]) {
                    DbgPrint("[AmtPtp] IdentityBreak SUPPRESSED (glitch) pool=%Iu "
                             "candX=%u candY=%u lastX=%u lastY=%u\n",
                             p, Candidates->Candidates[ci].X, Candidates->Candidates[ci].Y,
                             Pool[p].ReportX, Pool[p].ReportY);
                }
#endif
            } else {
                OutResult->NewIdentity[ci] = FALSE;
            }
        }
    }

    // Unclaimed -> lift.
    for (size_t p = 0; p < MAX_CONTACTS; p++) {
        if (Pool[p].State != CONTACT_ACTIVE)
            continue;
        if (!poolClaimedFinal[p]) {
            OutResult->UnmatchedPoolIndices[OutResult->UnmatchedCount++] = p;
        }
    }
}