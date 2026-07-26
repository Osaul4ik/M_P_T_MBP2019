// Palm.c - Palm classification. See Palm.h.
//
// Scoring logic copied verbatim from the old AmtClassifyPalm (Match.c) -
// the thresholds here were tuned against real hardware behavior and are
// deliberately NOT touched by this refactor. Only the function boundary
// changed: this now takes raw geometry fields directly instead of a
// TRACKPAD_FINGER pointer, so it has zero dependency on the wire format.

#include "Driver.h"
#include "Palm.h"

#define PALM_LARGE_MAJOR    380
#define PALM_LARGE_RATIO    180  // major*100/minor. Real palm is elongated
                                 // (wide flat blob); a fat but round finger
                                 // pad has Major and Minor growing together,
                                 // so ratio stays near 100-150. Without this
                                 // gate a finger pad that crosses
                                 // PALM_LARGE_MAJOR was blanking the whole
                                 // frame (PALM_LARGE) even with zero
                                 // corroboration - unlike PALM_LOCAL below,
                                 // which already requires ratio/edge signal.
#define PALM_SCORE_THRESH   55   // was 45 - raised so a wide/flat fingertip
                                 // pad (large Major, but not elongated/
                                 // edge-adjacent like a real palm) needs
                                 // more corroborating signal before it's
                                 // suppressed.
#define PALM_MIN_MAJOR  80   // мінімальний major для підозри на долоню
#define PALM_MIN_MINOR  40   // мінімальний minor для підозри на долоню

// Thin hard cutoff right at the physical bottom edge - the strip where
// the heel of the hand rests against the laptop's chassis when leaning
// on it. No real finger ever deliberately lands this close to the
// physical edge, so this slice is rejected unconditionally, regardless
// of size/shape. Kept narrow on purpose (large divisor = thin strip) -
// this is NOT the same as the wider, size-gated scored zone below,
// which still lets real small touches through further up from the edge.
#define BOTTOM_HARD_CUTOFF_DIVISOR 16

static inline INT
AmtPalmRawToInteger(_In_ USHORT x)
{
    return (signed short)x;
}

PALM_CLASS
AmtPalmClassify(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ INT                          NormX,
    _In_ INT                          NormY
)
{
    INT yRangeFull = DevInfo->y.max - DevInfo->y.min;
    if (NormY > (yRangeFull - yRangeFull / BOTTOM_HARD_CUTOFF_DIVISOR))
        return PALM_LOCAL;

    INT major = AmtPalmRawToInteger(Major);
    INT minor = AmtPalmRawToInteger(Minor);
    
    if (major < PALM_MIN_MAJOR && minor < PALM_MIN_MINOR) {
        return PALM_NONE;
    }
    INT score = 0;

    if (major <= 0 && minor <= 0)
        return PALM_NONE;

    if (major >= PALM_LARGE_MAJOR) {
        // minor <= 0 means the sensor gave no shape info at all for this
        // contact - treat as palm (can't prove it's a round finger pad).
        // Otherwise require the elongation ratio, so a fat round pad falls
        // through to the score path below instead of an instant blank.
        if (minor <= 0)
            return PALM_LARGE;

        INT largeRatio = major * 100 / minor;
        if (largeRatio > PALM_LARGE_RATIO)
            return PALM_LARGE;
    }

    if      (major > 300) score += 35;   // was 260
    else if (major > 220) score += 15;   // was 190
    else if (major > 150) score +=  8;   // was 130

    if (minor > 0 && major > 120) {
        INT ratio = major * 100 / minor;
        if      (ratio > 1200) score += 30;
        else if (ratio >  900) score += 20;
        else if (ratio >  600) score += 10;
    }

    if (major > 130) {
        INT xRange   = DevInfo->x.max - DevInfo->x.min;
        INT yRange   = DevInfo->y.max - DevInfo->y.min;

        // Per-side edge score divisors (smaller divisor = wider zone).
        // Top stays tight - legitimate taps/scroll gestures land close
        // to the top edge often enough that widening it would cost
        // real input. Left/right and bottom are widened - palm contact
        // is far more likely there. Bottom's zone here is wider than
        // the thin hard cutoff above it - this is the size-gated
        // transition band: only major>130 contacts get scored, so a
        // normal small fingertip passing through is never affected,
        // only wide/flat palm-shaped ones.
        #define EDGE_DIVISOR_TOP     28
        #define EDGE_DIVISOR_LEFT    12
        #define EDGE_DIVISOR_RIGHT   12
        #define EDGE_DIVISOR_BOTTOM   6

        INT edgeTop    = yRange / EDGE_DIVISOR_TOP;
        INT edgeLeft   = xRange / EDGE_DIVISOR_LEFT;
        INT edgeRight  = xRange / EDGE_DIVISOR_RIGHT;
        INT edgeBottom = yRange / EDGE_DIVISOR_BOTTOM;

        if (NormX < edgeLeft || NormX > (xRange - edgeRight) ||
            NormY < edgeTop  || NormY > (yRange - edgeBottom))
            score += 10;
    }

    return (score >= PALM_SCORE_THRESH) ? PALM_LOCAL : PALM_NONE;
}
