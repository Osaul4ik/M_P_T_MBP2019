// Classify palm vs finger using simple geometry heuristics.

#include "Driver.h"
#include "Palm.h"

#define PALM_LARGE_MAJOR    380
#define PALM_LARGE_RATIO    180  // Real palms are elongated; round pads are not.
#define PALM_SCORE_THRESH   55   // Require stronger evidence before suppressing a wide pad.
#define PALM_MIN_MAJOR  80   // мінімальний major для підозри на долоню
#define PALM_MIN_MINOR  40   // мінімальний minor для підозри на долоню

PALM_CLASS
AmtPalmClassify(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ INT                          NormX,
    _In_ INT                          NormY
)
{
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

    if (major > 130) {
        INT xRange   = DevInfo->x.max - DevInfo->x.min;
        INT yRange   = DevInfo->y.max - DevInfo->y.min;

        // Edge weighting favors the top edge for taps and widens the lower edges for palms.
        #define EDGE_DIVISOR_TOP     28   // tight zone, near NormY=0
        #define EDGE_DIVISOR_LEFT    7
        #define EDGE_DIVISOR_RIGHT   7
        #define EDGE_DIVISOR_BOTTOM  4    // wide zone, near NormY=yRange

        INT edgeLeft   = xRange / EDGE_DIVISOR_LEFT;
        INT edgeRight  = xRange / EDGE_DIVISOR_RIGHT;
        INT edgeTop    = yRange / EDGE_DIVISOR_TOP;
        INT edgeBottom = yRange / EDGE_DIVISOR_BOTTOM;

        if (NormX < edgeLeft || NormX > (xRange - edgeRight) ||
            NormY < edgeTop  || NormY > (yRange - edgeBottom))
            score += 10;
    }

    return (score >= PALM_SCORE_THRESH) ? PALM_LOCAL : PALM_NONE;
}