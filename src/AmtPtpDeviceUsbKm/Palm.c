// Classify palm vs finger using simple geometry heuristics.

#include "Driver.h"
#include "Palm.h"

// ============================================================================
// All thresholds below used to be compile-time #defines. They are now read
// from AMT_PALM_CONFIG (Config parameter) so AmtPtpConfigGui can tune them
// live via IOCTL_AMT_PTP_SET_PALM_CONFIG - no reboot/reinstall needed. The
// AMT_PALM_CONFIG_DEFAULT_INIT values in Public.h are byte-for-byte the old
// hardcoded numbers, so a fresh install behaves identically to before.
//
// Edge-zone size is expressed in PERMILLE (parts-per-1000 = 0.1%) of the
// pad's usable width (left/right) or height (top/bottom) - percent-of-range
// is the portable knob across different trackpad models (e.g. 2015 MacBook
// Air vs a T2 machine have different physical-size-to-sensor-range ratios).
// ============================================================================

// MICRO-OPT: edge factors are precomputed when the palm config changes.
// The hot per-contact path only does multiply+shift; no permille division.
static inline INT
AmtPalmEdgeWidth(_In_ INT Range, _In_ LONGLONG FactorQ32)
{
    return (INT)(((LONGLONG)Range * FactorQ32) >> AMT_RUNTIME_FIXED_SHIFT);
}

static BOOLEAN
AmtPalmInEdgeZone(
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ const AMT_PALM_RUNTIME*      Runtime,
    _In_ INT                          NormX,
    _In_ INT                          NormY
)
{
    INT xRange = DevInfo->x.max - DevInfo->x.min;
    INT yRange = DevInfo->y.max - DevInfo->y.min;

    INT edgeLeft   = AmtPalmEdgeWidth(xRange, Runtime->EdgeFactorLeftQ32);
    INT edgeRight  = AmtPalmEdgeWidth(xRange, Runtime->EdgeFactorRightQ32);
    INT edgeTop    = AmtPalmEdgeWidth(yRange, Runtime->EdgeFactorTopQ32);
    INT edgeBottom = AmtPalmEdgeWidth(yRange, Runtime->EdgeFactorBottomQ32);

    return (BOOLEAN)(NormX < edgeLeft || NormX > (xRange - edgeRight) ||
                      NormY < edgeTop  || NormY > (yRange - edgeBottom));
}

PALM_CLASS
AmtPalmClassify(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ const AMT_PALM_CONFIG*       Config,
    _In_ const AMT_PALM_RUNTIME*      Runtime,
    _In_ INT                          NormX,
    _In_ INT                          NormY,
    _In_ BOOLEAN                      IsBirth
)
{
    // Hard reject: a contact that FIRST APPEARS inside the edge zone is
    // treated as palm outright, regardless of shape/size. This runs before
    // the small-contact early-out below on purpose - a birth in the zone
    // is rejected even if it's finger-sized, since the whole point is to
    // suppress accidental edge touches, not just wide palm-shaped ones.
    // Contacts already being tracked that merely move through the zone are
    // NOT affected - only IsBirth is checked here.
    if (IsBirth && AmtPalmInEdgeZone(DevInfo, Runtime, NormX, NormY)) {
        return PALM_LOCAL;
    }

    INT major = AmtRawToSignedInt(Major);
    INT minor = AmtRawToSignedInt(Minor);

    INT palmMinMajor  = (INT)Config->PalmMinMajor;
    INT palmMinMinor  = (INT)Config->PalmMinMinor;
    INT palmLargeMajor = (INT)Config->PalmLargeMajor;
    INT palmLargeRatio = (INT)Config->PalmLargeRatio;
    INT palmScoreThresh = (INT)Config->PalmScoreThresh;

    if (major < palmMinMajor && minor < palmMinMinor) {
        return PALM_NONE;
    }
    INT score = 0;

    if (major <= 0 && minor <= 0)
        return PALM_NONE;

    if (major >= palmLargeMajor) {
        // Treat missing shape data as palm; otherwise require a strong elongation signal.
        if (minor <= 0)
            return PALM_LARGE;

        // MICRO-OPT: division replaced with cross-multiplication.
        // floor(A/B) > C  <=>  A >= (C+1)*B  for non-negative A,B,C, B>0.
        // Exactly equivalent to (major*100/minor) > PalmLargeRatio,
        // just without the runtime div. INT64 guards against overflow
        // (major/minor are USHORT-derived, no realistic overflow risk,
        // but kept explicit for clarity).
        if ((INT64)major * 100 >= (INT64)(palmLargeRatio + 1) * minor)
            return PALM_LARGE;
    }

    if      (major > 300) score += 35;   // was 260
    else if (major > 220) score += 15;   // was 190
    else if (major > 150) score +=  8;   // was 130

    if (minor > 0 && major > 120) {
        // MICRO-OPT: same division->cross-multiplication rewrite as above.
        // (major*100/minor) > C  <=>  major*100 >= (C+1)*minor.
        INT64 major100 = (INT64)major * 100;
        if      (major100 >= 1201LL * minor) score += 30;
        else if (major100 >=  901LL * minor) score += 20;
        else if (major100 >=  601LL * minor) score += 10;
    }

    // Soft edge bonus for continuations (or births that weren't caught by
    // the hard-reject above, e.g. a birth reported with major==0 for one
    // frame). Kept as a secondary signal on top of the hard reject.
    if (major > 130 && AmtPalmInEdgeZone(DevInfo, Runtime, NormX, NormY))
        score += 10;

    return (score >= palmScoreThresh) ? PALM_LOCAL : PALM_NONE;
}