// Match.h - Cost-based matcher between RawFrame contacts and ACTIVE_CONTACT pool.
// Slot index is a matching hint only, never direct index.

#pragma once

#include "PTPCore.h"
#include "ActiveContact.h"

EXTERN_C_START

// Candidate contact produced from the current frame.
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
    UCHAR   TipDropApplied;  // non-zero when X/Y is stale (debounce bridge).
                             // Position provenance only - does NOT drive
                             // Confidence (see Unconfirmed below).

    // TRUE for a brand-new, below-tip-threshold contact with no pool anchor.
    BOOLEAN Unconfirmed;
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
    // Parallel to Candidates[]. Pool index or MATCH_NO_CORRESPONDENCE.
    size_t  CorrespondingPoolIndex[PTP_MAX_CONTACT_POINTS];

    // Identity broken (lift + birth) despite correspondence.
    BOOLEAN NewIdentity[PTP_MAX_CONTACT_POINTS];

    // Pool indices with no corresponding candidate (should lift).
    size_t  UnmatchedPoolIndices[MAX_CONTACTS];
    UCHAR   UnmatchedCount;
} MATCH_RESULT;

// Build the candidate set for matching.
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

// Match contacts greedily and reject bad or stale pairs.
VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  PerfFrequencyHz,
    _Out_ MATCH_RESULT*                              OutResult
);

EXTERN_C_END