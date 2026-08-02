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
    UCHAR   TipDropApplied;  // non-zero when X/Y is stale (debounce bridge).
                             // Position provenance only - does NOT drive
                             // Confidence anymore, see Unconfirmed below.

    // FIX (soft-tap Confidence flicker / phantom-finger birth): whether
    // this candidate's very EXISTENCE (not just its position) is still
    // unverified. TRUE only for a below-tip-threshold candidate with no
    // pool anchor - i.e. this exact physical contact has never been
    // observed before and is too small on its own to be sure it isn't
    // sensor noise. FALSE for every other case, INCLUDING an anchored
    // below-threshold bridge candidate (isStationary or not) - once a
    // contact has matched an existing pool entry at all, its existence is
    // established and it should read as a confident finger to Windows'
    // PTP stack regardless of whether it happens to be holding still this
    // exact frame (a real tap/soft-press is *expected* to be nearly
    // stationary - penalizing stillness here was backwards). See
    // AmtMatchBuildCandidates for where this is set, and PTPCore.c's
    // reportConfident derivation for where it's consumed.
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
    // Parallel to MATCH_CANDIDATE_SET.Candidates[]. Pool index or
    // MATCH_NO_CORRESPONDENCE if this candidate should birth new contact.
    size_t  CorrespondingPoolIndex[PTP_MAX_CONTACT_POINTS];

    // Identity broken (lift + birth) despite correspondence.
    BOOLEAN NewIdentity[PTP_MAX_CONTACT_POINTS];

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

// Cost-based correspondence. Greedy min-cost, N,M<=5. Rejected on spatial/
// time gap or IdentityBreak. Ties within MATCH_TIE_EPSILON_SQ break first
// on LastSlotHint match, then (if that's also tied) on touch geometry/
// pressure similarity - see MATCH_SHAPE_TIE_EPSILON in Match.c.
VOID
AmtMatchCorrespond(
    _In_  const MATCH_CANDIDATE_SET*               Candidates,
    _In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT*  Pool,
    _In_  LONGLONG                                  NowQpc,
    _In_  LONGLONG                                  PerfFrequencyHz,
    _Out_ MATCH_RESULT*                              OutResult
);

EXTERN_C_END