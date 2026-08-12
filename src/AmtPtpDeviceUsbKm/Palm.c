// Classify palm vs finger using simple geometry heuristics.

#include "Driver.h"
#include "Palm.h"

#define PALM_LARGE_MAJOR    380
#define PALM_LARGE_RATIO    180  // Real palms are elongated; round pads are not.
#define PALM_SCORE_THRESH   55   // Require stronger evidence before suppressing a wide pad.
#define PALM_MIN_MAJOR  80   // мінімальний major для підозри на долоню
#define PALM_MIN_MINOR  40   // мінімальний minor для підозри на долоню

// ============================================================================
// Edge-zone size, in PERMILLE (parts per 1000 = 0.1%) of the pad's usable
// width (left/right) or height (top/bottom). TUNE HERE per trackpad model -
// different pads (e.g. 2015 MacBook Air vs a T2 machine) have different
// physical-size-to-sensor-range ratios, so a fixed absolute or divisor-based
// zone can end up way too big/small on a different model. Percent of range
// is the portable knob.
//
//   36   ->  3.6%
//   143  -> 14.3%
//   250  -> 25.0%
//
// Old divisor equivalents (kept as reference): /28 ~= 36 permille,
// /7 ~= 143 permille, /4 = 250 permille exactly.
#define EDGE_PERMILLE_TOP     36   // tight zone, near NormY=0
#define EDGE_PERMILLE_LEFT   143
#define EDGE_PERMILLE_RIGHT  143
#define EDGE_PERMILLE_BOTTOM 250   // wide zone, near NormY=yRange
// ============================================================================

// MICRO-OPT: "range * permille / 1000" replaced with fixed-point
// "range * factor >> SHIFT" - no runtime division (same reasoning as the
// ratio cross-multiplication above: avoids a div/64-bit-div-helper per
// contact per frame). factor = (permille << SHIFT) / 1000 is a division of
// two compile-time constants, folded away entirely by the compiler - only
// the multiply+shift below runs at runtime.
#define EDGE_FIXED_SHIFT 16
#define EDGE_FACTOR(permille) (((INT64)(permille) << EDGE_FIXED_SHIFT) / 1000)

#define EDGE_FACTOR_TOP    EDGE_FACTOR(EDGE_PERMILLE_TOP)
#define EDGE_FACTOR_LEFT   EDGE_FACTOR(EDGE_PERMILLE_LEFT)
#define EDGE_FACTOR_RIGHT  EDGE_FACTOR(EDGE_PERMILLE_RIGHT)
#define EDGE_FACTOR_BOTTOM EDGE_FACTOR(EDGE_PERMILLE_BOTTOM)

static inline INT
AmtPalmEdgeWidth(_In_ INT Range, _In_ INT64 Factor)
{
    return (INT)(((INT64)Range * Factor) >> EDGE_FIXED_SHIFT);
}

static BOOLEAN
AmtPalmInEdgeZone(
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ INT                          NormX,
    _In_ INT                          NormY
)
{
    INT xRange = DevInfo->x.max - DevInfo->x.min;
    INT yRange = DevInfo->y.max - DevInfo->y.min;

    INT edgeLeft   = AmtPalmEdgeWidth(xRange, EDGE_FACTOR_LEFT);
    INT edgeRight  = AmtPalmEdgeWidth(xRange, EDGE_FACTOR_RIGHT);
    INT edgeTop    = AmtPalmEdgeWidth(yRange, EDGE_FACTOR_TOP);
    INT edgeBottom = AmtPalmEdgeWidth(yRange, EDGE_FACTOR_BOTTOM);

    return (BOOLEAN)(NormX < edgeLeft || NormX > (xRange - edgeRight) ||
                      NormY < edgeTop  || NormY > (yRange - edgeBottom));
}

PALM_CLASS
AmtPalmClassify(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
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
    if (IsBirth && AmtPalmInEdgeZone(DevInfo, NormX, NormY)) {
        return PALM_LOCAL;
    }

    INT major = AmtRawToSignedInt(Major);
    INT minor = AmtRawToSignedInt(Minor);

    if (major < PALM_MIN_MAJOR && minor < PALM_MIN_MINOR) {
        return PALM_NONE;
    }
    INT score = 0;

    if (major <= 0 && minor <= 0)
        return PALM_NONE;

    if (major >= PALM_LARGE_MAJOR) {
        // Treat missing shape data as palm; otherwise require a strong elongation signal.
        if (minor <= 0)
            return PALM_LARGE;

        // MICRO-OPT: division replaced with cross-multiplication.
        // floor(A/B) > C  <=>  A >= (C+1)*B  for non-negative A,B,C, B>0.
        // Exactly equivalent to (major*100/minor) > PALM_LARGE_RATIO,
        // just without the runtime div. INT64 guards against overflow
        // (major/minor are USHORT-derived, no realistic overflow risk,
        // but kept explicit for clarity).
        if ((INT64)major * 100 >= (INT64)(PALM_LARGE_RATIO + 1) * minor)
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
    if (major > 130 && AmtPalmInEdgeZone(DevInfo, NormX, NormY))
        score += 10;

    return (score >= PALM_SCORE_THRESH) ? PALM_LOCAL : PALM_NONE;
}