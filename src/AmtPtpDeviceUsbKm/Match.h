// Match.h - Cost-based matcher between RawFrame contacts and ACTIVE_CONTACT pool.
// Slot index is a matching hint only, never direct index.

#pragma once

#include "PTPCore.h"
#include "ActiveContact.h"

EXTERN_C_START

// Palm/tip-debounce-classified candidate. Dense list, not slot-indexed.
typedef struct _MATCH_CANDIDATE
{
    USHORT  SlotIndex;      // hw slot - HINT only
    USHORT  X;
    USHORT  Y;
    USHORT  Major;          // touch_major, raw - matching tie-break only
    USHORT  Minor;          // touch_minor, raw - matching tie-break only
    USHORT  Pressure;       // raw ADC units - matching tie-break only
    BOOLEAN PalmLocal;       // excluded from matching/reporting
    BOOLEAN IdentityBreak;   // firmware origin==0 signal
    UCHAR   TipDropApplied;  // non-zero when X/Y is stale (debounce bridge)
} MATCH_CANDIDATE;

typedef struct _MATCH_CANDIDATE_SET
{
    UCHAR           Count;
    MATCH_CANDIDATE Candidates[PTP_MAX_CONTACT_POINTS];
} MATCH_CANDIDATE_SET;

// Correspondence result: pool index or MATCH_NO_CORRESPONDENCE.
#define MATCH_NO_CORRESPONDENCE  ((size_t)-1)

typedef struct _MATCH_RESULT
{
    // Parallel to MATCH_CANDIDATE_SET.Candidates[]. Pool index or
    // MATCH_NO_CORRESPONDENCE if this candidate should birth new contact.
    size_t  CorrespondingPoolIndex[PTP_MAX_CONTACT_POINTS];

    // Identity broken (lift + birth) despite correspondence.
    BOOLEAN NewIdentity[PTP_MAX_CONTACT_POINTS];

    // TRUE when this candidate is matched (CorrespondingPoolIndex valid),
    // carries IdentityBreak, and the break was suppressed (NewIdentity ==
    // FALSE) ONLY because it fell inside the wide multi-finger-glitch
    // window (> IDENTITY_BREAK_MAX_PLAUSIBLE_JUMP, still <
    // MATCH_MAX_CONTINUATION_DELTA) - see AmtMatchCorrespond. In BOTH
    // readings of that situation - a genuine BCM5974 renumbering glitch
    // (the candidate's coordinates are one-frame garbage) or a second,
    // genuinely new finger that happened to be the only feasible partner
    // left for this pool slot (no cheap match blocked it) - the
    // candidate's raw X/Y must NOT be trusted as this pool entry's new
    // position: the caller (Ptpcore.c Phase C) must hold the pool entry's
    // existing ReportX/Y instead of feeding it cand->X/Y, so a suppressed
    // break can never itself become a silent multi-thousand-unit cursor
    // teleport - which is exactly what happens if it's left unguarded,
    // regardless of which of the two interpretations was true this frame.
    BOOLEAN PositionSuppressed[PTP_MAX_CONTACT_POINTS];

    // Pool indices with no corresponding candidate (should lift).
    size_t  UnmatchedPoolIndices[MAX_CONTACTS];
    UCHAR   UnmatchedCount;
} MATCH_RESULT;

// Build candidate set: palm classification + tip-size debounce.
// Below-tip with no anchor = full-confidence birth candidate.
VOID
AmtMatchBuildCandidates(
    _In_  const RAW_FRAME*                        RawFrame,
    _In_  const struct BCM5974_CONFIG*             DevInfo,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool,
    _Out_ MATCH_CANDIDATE_SET*                     OutCandidates,
    _Out_ BOOLEAN*                                 LargePalmDetected
);

// Max time gap for same-ContactID continuation (150ms).
#define MATCH_MAX_TIME_DELTA_100NS (150LL * 10000LL)

// Cost-based correspondence. Exact max-cardinality, min-cost bipartite
// assignment via bounded backtracking (N,M<=5, see AmtMatchSearch in
// Match.c) - not a greedy nearest-neighbor pass, so a jitter-sized cost
// difference between two crossing candidates can't make the wrong one
// grab a pool slot before the right one is considered. The search now
// also branch-and-bound prunes subtrees that provably cannot beat the
// best assignment found so far (see AmtMatchSearch) - same exact result,
// fewer leaves visited. Pairs failing the spatial/time gap check are
// excluded from the search entirely (never assignable). The ranking cost
// compares each candidate against the pool entry's DEAD-RECKONED
// position (last report position extrapolated by ACTIVE_CONTACT's
// VelocityX/Y over the elapsed time since LastSeenQpc - see
// AmtMatchPredictPosition in Match.c) instead of the stale last-report
// position alone, so a fast, steadily-moving contact isn't penalized for
// having moved since it was last seen. This only affects ranking/tie-
// breaking; the spatial/time feasibility gate still uses the actual last
// report position, unchanged. Ties in total cost break on total
// LastSlotHint agreement, then on total touch geometry/pressure
// similarity - see AmtMatchShapeDistance in Match.c, which is now
// normalized against DevInfo's calibrated pressure/width ranges instead
// of comparing raw ADC units directly.
VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  const struct BCM5974_CONFIG*               DevInfo,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  PerfFrequencyHz,
    _Out_ MATCH_RESULT*                              OutResult
);

EXTERN_C_END