// ActiveContact.c - Contact lifecycle FSM.

#include "Driver.h"
#include "ActiveContact.h"

// Pointer/scroll thresholds, deadzones, smoothing coefficients and scroll
// scale factors are runtime-configurable through AMT_*_CONFIG.

typedef enum _CONTACT_VELOCITY_BUCKET
{
    VELOCITY_UNKNOWN = 0,  // no prior timestamp
    VELOCITY_SLOW,
    VELOCITY_MEDIUM,
    VELOCITY_FAST,
} CONTACT_VELOCITY_BUCKET;

static inline INT
AmtMulQ32Round(_In_ INT Value, _In_ LONGLONG CoeffQ32)
{
    LONGLONG product = (LONGLONG)Value * CoeffQ32;
    const LONGLONG half = 1LL << (AMT_RUNTIME_FIXED_SHIFT - 1);

    if (product >= 0)
        return (INT)((product + half) >> AMT_RUNTIME_FIXED_SHIFT);

    product = -product;
    return -(INT)((product + half) >> AMT_RUNTIME_FIXED_SHIFT);
}

static inline INT
AmtQ32ProductToIntRound(_In_ LONGLONG Product)
{
    const LONGLONG half = 1LL << (AMT_RUNTIME_FIXED_SHIFT - 1);

    if (Product >= 0)
        return (INT)((Product + half) >> AMT_RUNTIME_FIXED_SHIFT);

    Product = -Product;
    return -(INT)((Product + half) >> AMT_RUNTIME_FIXED_SHIFT);
}

// Lower smoothing reduces slow-speed jitter.

// Continuous alpha endpoints and scroll scale coefficients are supplied by
// precomputed runtime state; raw config remains the authoritative user-facing
// representation.

static inline USHORT
AmtContactSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal, _In_ INT alphaNum,
    _In_ INT alphaDen)
{
    // Blend the new sample with the previous report.
    INT blended = ((INT)rawVal * alphaNum +
                   (INT)prevVal * (alphaDen - alphaNum)) /
                  alphaDen;
    return (USHORT)blended;
}

// Estimate motion speed and the continuous smoothing alpha from a single
// distance/unitsPerSec computation, shared between the two consumers below
// instead of each recomputing dx/dy/distance separately.
//
// Bucket uses FastThresholdUnitsPerSec (caller picks cursor vs. scroll
// threshold). Alpha always ramps against the cursor thresholds
// (VELOCITY_SLOW/FAST_UNITS_PER_SEC) regardless of which threshold the
// bucket used - it's only ever consumed by the caller on the non-gesture
// (single-finger) path, but is always computed and written here so
// OutAlphaNum is unconditionally valid on return.
static CONTACT_VELOCITY_BUCKET
AmtContactEvaluateVelocity(
    _In_  USHORT   rawX,
    _In_  USHORT   rawY,
    _In_  USHORT   prevX,
    _In_  USHORT   prevY,
    _In_  LONGLONG DtQpcTicks,
    _In_  LONGLONG PerfFrequencyHz,
    _In_  LONGLONG FastThresholdUnitsPerSec,
    _In_  LONGLONG        SlowThresholdUnitsPerSec,
    _In_  LONGLONG        AlphaFastThresholdUnitsPerSec,
    _In_  const AMT_POINTER_RUNTIME* PointerRuntime,
    _In_  BOOLEAN         ComputeAlpha,
    _Out_ INT*     OutAlphaNum
)
{
    if (DtQpcTicks <= 0 || PerfFrequencyHz <= 0) {
        // No timestamp basis yet - same default as before: raw/unknown.
        *OutAlphaNum = PointerRuntime->CursorSmoothingAlphaDen;
        return VELOCITY_UNKNOWN;
    }

    INT dx = AmtAbsDelta((INT)rawX, (INT)prevX);
    INT dy = AmtAbsDelta((INT)rawY, (INT)prevY);
    INT distance = (dx > dy) ? dx : dy;

    LONGLONG unitsPerSec = ((LONGLONG)distance * PerfFrequencyHz) / DtQpcTicks;

    if (!ComputeAlpha) {
        *OutAlphaNum = PointerRuntime->CursorSmoothingAlphaDen;
        if (unitsPerSec <= SlowThresholdUnitsPerSec)
            return VELOCITY_SLOW;
        if (unitsPerSec >= FastThresholdUnitsPerSec)
            return VELOCITY_FAST;
        return VELOCITY_MEDIUM;
    }

    CONTACT_VELOCITY_BUCKET bucket;
    if (unitsPerSec <= SlowThresholdUnitsPerSec) {
        bucket = VELOCITY_SLOW;
    } else if (unitsPerSec >= FastThresholdUnitsPerSec) {
        bucket = VELOCITY_FAST;
    } else {
        bucket = VELOCITY_MEDIUM;
    }

    // Continuous ramp between the configured slow alpha numerator (at/below
    // the slow velocity threshold) and the configured denominator (at/above
    // the fast velocity threshold, bit-identical to raw), so a speed
    // anywhere in between gets a proportionally lighter blend instead
    // of snapping between two fixed states at a single threshold.
    //
    // On gesture frames ComputeAlpha is FALSE, so the alpha-ramp division is
    // skipped entirely; the caller does not consume alpha for gestures.
    if (unitsPerSec <= SlowThresholdUnitsPerSec) {
        *OutAlphaNum = PointerRuntime->CursorSmoothingAlphaNumSlow;
    } else if (unitsPerSec >= AlphaFastThresholdUnitsPerSec) {
        *OutAlphaNum = PointerRuntime->CursorSmoothingAlphaDen;
    } else {
        LONGLONG offset = unitsPerSec - SlowThresholdUnitsPerSec;
        INT ramp = AmtMulQ32Round((INT)offset, PointerRuntime->CursorSmoothingSlopeQ32);
        INT alpha = PointerRuntime->CursorSmoothingAlphaNumSlow + ramp;
        if (alpha > PointerRuntime->CursorSmoothingAlphaDen)
            alpha = PointerRuntime->CursorSmoothingAlphaDen;
        *OutAlphaNum = alpha;
    }

    return bucket;
}

static inline INT
AmtContactDeadzoneForVelocity(
    _In_ CONTACT_VELOCITY_BUCKET Velocity,
    _In_ BOOLEAN GestureActive,
    _In_ const AMT_POINTER_CONFIG* PointerConfig,
    _In_ const AMT_SCROLL_CONFIG* ScrollConfig)
{
    if (GestureActive)
        return (INT)ScrollConfig->Deadzone;

    if (Velocity == VELOCITY_SLOW)
        return (INT)PointerConfig->CursorDeadzoneSlow;
    if (Velocity == VELOCITY_FAST)
        return (INT)PointerConfig->CursorDeadzoneFast;
    return (INT)PointerConfig->CursorDeadzone;
}

// Assign a monotonic contact ID.
static inline ULONG
AmtContactAssignId(_Inout_ ULONG* NextContactId)
{
    ULONG id = ++(*NextContactId);
    if (id == 0) {
        // 0 is reserved; skip it.
        id = ++(*NextContactId);
    }
    return id;
}

VOID
AmtContactPoolInit(_Out_writes_(MAX_CONTACTS) PACTIVE_CONTACT Pool)
{
    RtlZeroMemory(Pool, sizeof(ACTIVE_CONTACT) * MAX_CONTACTS);
}

size_t
AmtContactPoolFindFree(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool)
{
    for (size_t i = 0; i < MAX_CONTACTS; i++) {
        if (Pool[i].State == CONTACT_FREE)
            return i;
    }
    return MAX_CONTACTS; // pool exhausted (should not happen)
}

// Shared field init for AmtContactBirth / AmtContactBirthWithRetapSmoothing -
// identical in every field except the X/Y source and RetapSeeded.
static inline VOID
AmtContactBirthCommon(
    _Inout_ PACTIVE_CONTACT c,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          slotHint,
    _In_    BOOLEAN         retapSeeded
)
{
    c->State             = CONTACT_ACTIVE;
    c->ContactID          = AmtContactAssignId(NextContactId);
    c->ReportX            = x;
    c->ReportY            = y;
    c->HystX              = x;
    c->HystY              = y;
    c->WasInGesture       = FALSE;
    c->PendingFirstSample = TRUE;
    c->RetapSeeded        = retapSeeded;
    c->ScrollRemX          = 0;
    c->ScrollRemY          = 0;
    c->ScrollFilteredX     = 0;
    c->ScrollFilteredY     = 0;
    c->LastSlotHint        = slotHint;
    c->LastSeenQpc         = 0; // set by first AmtContactUpdate
    c->LastMajor           = 0;
    c->LastMinor           = 0;
    c->LastPressure        = 0;
    c->FramesAlive         = 1; // birth frame counts as 1
}

VOID
AmtContactBirth(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          slotHint
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_FREE);
#endif

    AmtContactBirthCommon(c, NextContactId, x, y, slotHint, /* retapSeeded */ FALSE);
}

// Seeds EMA baseline so the cursor doesn't jump on re-tap.
VOID
AmtContactBirthWithRetapSmoothing(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          RecentLiftX,
    _In_    USHORT          RecentLiftY,
    _In_    USHORT          slotHint
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_FREE);
#endif

    AmtContactBirthCommon(c, NextContactId, RecentLiftX, RecentLiftY, slotHint,
                           /* retapSeeded */ TRUE); // preserve seed on first update
}

BOOLEAN
AmtContactIsRecentLiftNearby(
    _In_ LONGLONG LiftQpc,
    _In_ USHORT   LiftX,
    _In_ USHORT   LiftY,
    _In_ LONGLONG NowQpc,
    _In_ LONGLONG PerfFrequencyHz,
    _In_ USHORT   CandX,
    _In_ USHORT   CandY
)
{
    if (LiftQpc == 0)
        return FALSE; // no recent lift recorded

    if (NowQpc < LiftQpc)
        return FALSE; // QPC must be monotonic

    if (PerfFrequencyHz <= 0)
        return FALSE; // fail closed

    LONGLONG deltaTicks  = NowQpc - LiftQpc;
    LONGLONG windowTicks = (RETAP_WINDOW_100NS * PerfFrequencyHz) / 10000000LL;

    if (deltaTicks > windowTicks)
        return FALSE;

    INT dx = AmtAbsDelta((INT)CandX, (INT)LiftX);
    INT dy = AmtAbsDelta((INT)CandY, (INT)LiftY);

    return (dx <= RETAP_MAX_DISTANCE) && (dy <= RETAP_MAX_DISTANCE);
}

VOID
AmtContactKill(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_ACTIVE || c->State == CONTACT_GRACE);
#endif

    *OldContactID = c->ContactID;
    *OldX         = c->ReportX;
    *OldY         = c->ReportY;

    RtlZeroMemory(c, sizeof(ACTIVE_CONTACT));
}

// Synthetic rebirth keeps smoothing state while swapping the contact ID.
VOID
AmtContactRebindIdentity(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _Out_   ULONG*          OldContactID
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_ACTIVE);
#endif

    *OldContactID = c->ContactID;
    c->ContactID  = AmtContactAssignId(NextContactId);
}

VOID
AmtContactEnterGrace(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_ACTIVE);
#endif

    *OldContactID = c->ContactID;
    *OldX         = c->ReportX;
    *OldY         = c->ReportY;

    c->State = CONTACT_GRACE;
}

VOID
AmtContactExpireGrace(_Inout_ PACTIVE_CONTACT Pool, _In_ size_t index)
{
    PACTIVE_CONTACT c = &Pool[index];

#if DBG
    NT_ASSERT(c->State == CONTACT_GRACE);
#endif

    RtlZeroMemory(c, sizeof(ACTIVE_CONTACT));
}

BOOLEAN
AmtContactEvaluateDeadzone(
    _In_ const ACTIVE_CONTACT* Contact,
    _In_ USHORT                candX,
    _In_ USHORT                candY,
    _In_ INT                   ThresholdUnits
)
{
    if (ThresholdUnits <= 0) {
        return TRUE;
    }

    INT dx = AmtAbsDelta((INT)candX, (INT)Contact->HystX);
    INT dy = AmtAbsDelta((INT)candY, (INT)Contact->HystY);

    return (dx >= ThresholdUnits) || (dy >= ThresholdUnits);
}

static inline VOID
AmtContactCommitSample(
    _Inout_ PACTIVE_CONTACT Contact,
    _In_    USHORT          candX,
    _In_    USHORT          candY,
    _In_    BOOLEAN         passedDeadzone,
    _In_    BOOLEAN         aliveCountIsOne,
    _In_    BOOLEAN         gestureActive,
    _In_    CONTACT_VELOCITY_BUCKET Velocity,
    _In_    INT             alphaNum,
    _In_    BOOLEAN         commitIsRetapSeededFirstSample,
    _In_    const AMT_POINTER_CONFIG* PointerConfig,
    _In_    const AMT_SCROLL_CONFIG* ScrollConfig,
    _In_    const AMT_POINTER_RUNTIME* PointerRuntime,
    _In_    const AMT_SCROLL_RUNTIME* ScrollRuntime,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
)
{
    USHORT repX, repY;
    BOOLEAN velocityIsSlow = (Velocity == VELOCITY_SLOW);

    if (!passedDeadzone) {
        repX = Contact->ReportX;
        repY = Contact->ReportY;
    } else {
        Contact->HystX = candX;
        Contact->HystY = candY;

        if (gestureActive) {
            // Scroll frame: report ReportX/Y + a scaled fraction of the raw
            // delta, not the raw position itself - baseline for the next
            // frame's delta is this scaled result, so scaling never
            // compounds across frames. FAST swipes get a +35% boost over
            // SLOW/MEDIUM (see SCROLL_SCALE_*_FAST) so a quick fling covers
            // more distance per frame instead of feeling capped.
            INT dx = (INT)candX - (INT)Contact->ReportX;
            INT dy = (INT)candY - (INT)Contact->ReportY;

            LONGLONG scaleQ32 = (Velocity == VELOCITY_FAST)
                          ? ScrollRuntime->FastScaleQ32
                          : ScrollRuntime->BaseScaleQ32;
            INT scaledDx = AmtMulQ32Round(dx, scaleQ32);
            INT scaledDy = AmtMulQ32Round(dy, scaleQ32);

            if (ScrollRuntime->SmoothingAlphaQ32 != AMT_RUNTIME_FIXED_ONE) {
                LONGLONG alphaQ32 = ScrollRuntime->SmoothingAlphaQ32;
                LONGLONG prevQ32 = AMT_RUNTIME_FIXED_ONE - alphaQ32;
                LONGLONG mixX = (LONGLONG)scaledDx * alphaQ32 +
                                (LONGLONG)Contact->ScrollFilteredX * prevQ32;
                LONGLONG mixY = (LONGLONG)scaledDy * alphaQ32 +
                                (LONGLONG)Contact->ScrollFilteredY * prevQ32;
                scaledDx = AmtQ32ProductToIntRound(mixX);
                scaledDy = AmtQ32ProductToIntRound(mixY);
            }
            Contact->ScrollFilteredX = scaledDx;
            Contact->ScrollFilteredY = scaledDy;

            LONG newX = (LONG)Contact->ReportX + scaledDx;
            LONG newY = (LONG)Contact->ReportY + scaledDy;

            // Clamp both ends before truncating to USHORT.
            repX = (USHORT)(newX < 0 ? 0 : (newX > 0xFFFF ? 0xFFFF : newX));
            repY = (USHORT)(newY < 0 ? 0 : (newY > 0xFFFF ? 0xFFFF : newY));
        } else {
            // Retap-seeded first sample always eases in from the seed
            // (no real dt to measure velocity from yet - see
            // AmtContactUpdate). Everything else blends by the continuous
            // alpha the caller computed from measured velocity: heaviest
            // smoothing near-stationary, ramping linearly to a
            // bit-identical raw passthrough by the configured fast velocity.
            // Replaces the old hard SLOW/MEDIUM/FAST switch, which only
            // ever reported a fixed 3/8 blend or exactly raw with nothing
            // between - a deliberate slow move sitting right at the
            // threshold speed could flip frame-to-frame between the two.
            INT userAlpha = PointerRuntime->CursorSmoothingAlphaNum;
            INT effAlpha = commitIsRetapSeededFirstSample ? PointerRuntime->CursorSmoothingAlphaNumSlow :
                           ((alphaNum < userAlpha) ? alphaNum : userAlpha);

            repX = AmtContactSmoothCoord(candX, Contact->ReportX, effAlpha,
                                         PointerRuntime->CursorSmoothingAlphaDen);
            repY = AmtContactSmoothCoord(candY, Contact->ReportY, effAlpha,
                                         PointerRuntime->CursorSmoothingAlphaDen);

            if (PointerRuntime->CursorSpeedQ32 != AMT_RUNTIME_FIXED_ONE) {
                INT moveX = (INT)repX - (INT)Contact->ReportX;
                INT moveY = (INT)repY - (INT)Contact->ReportY;
                LONG nextX = (LONG)Contact->ReportX +
                              (LONG)AmtMulQ32Round(moveX, PointerRuntime->CursorSpeedQ32);
                LONG nextY = (LONG)Contact->ReportY +
                              (LONG)AmtMulQ32Round(moveY, PointerRuntime->CursorSpeedQ32);
                repX = (USHORT)(nextX < 0 ? 0 : (nextX > 0xFFFF ? 0xFFFF : nextX));
                repY = (USHORT)(nextY < 0 ? 0 : (nextY > 0xFFFF ? 0xFFFF : nextY));
            }

            Contact->ScrollRemX = 0;
            Contact->ScrollRemY = 0;
            Contact->ScrollFilteredX = 0;
            Contact->ScrollFilteredY = 0;
        }

        // Taint-clear rule unchanged from the old scheme: a contact that
        // was part of a gesture stays tainted through slow/ambiguous solo
        // movement, and only clears on a decisively fast frame (or a
        // gesture frame, or a retap-seeded one) - see the WasInGesture
        // comment in Activecontact.h.
        if (gestureActive || (!commitIsRetapSeededFirstSample && !velocityIsSlow)) {
            if (Contact->WasInGesture && aliveCountIsOne) {
                Contact->WasInGesture = FALSE;
            }
        }
    }

    Contact->ReportX = repX;
    Contact->ReportY = repY;
    Contact->PendingFirstSample = FALSE;
    Contact->RetapSeeded        = FALSE; // seed consumed exactly once

    *OutX = repX;
    *OutY = repY;
}

VOID
AmtContactUpdate(
    _Inout_ PACTIVE_CONTACT Contact,
    _In_    USHORT          rawX,
    _In_    USHORT          rawY,
    _In_    USHORT          major,
    _In_    USHORT          minor,
    _In_    USHORT          pressure,
    _In_    USHORT          slotHint,
    _In_    LONGLONG        nowQpc,
    _In_    LONGLONG        PerfFrequencyHz,
    _In_    BOOLEAN         aliveCountIsOne,
    _In_    BOOLEAN         gestureActive,
    _In_    const AMT_POINTER_CONFIG* PointerConfig,
    _In_    const AMT_SCROLL_CONFIG* ScrollConfig,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
)
{
#if DBG
    NT_ASSERT(Contact->State == CONTACT_ACTIVE);
#endif

    BOOLEAN passed;
    BOOLEAN retapSeededFirstSample =
        Contact->PendingFirstSample && Contact->RetapSeeded;

    // LastSeenQpc is still last frame's value here (overwritten below).
    // 0 = no previous sample; handled by the DtQpcTicks<=0 guard.
    LONGLONG prevQpc  = Contact->LastSeenQpc;
    LONGLONG dtTicks  = (prevQpc == 0) ? 0 : (nowQpc - prevQpc);

    // Scroll gestures classify FAST against their own threshold, decoupled
    // from single-finger cursor movement below. Alpha is only meaningful
    // on the non-gesture path (see AmtContactCommitSample) - it's still
    // The gesture path skips the alpha-ramp work inside AmtContactEvaluateVelocity.
    LONGLONG fastThreshold = gestureActive ? ScrollConfig->FastVelocity
                                            : PointerConfig->CursorFastVelocity;
    LONGLONG slowThreshold = PointerConfig->CursorSlowVelocity;
    LONGLONG alphaFastThreshold = PointerConfig->CursorFastVelocity;
    INT alphaNum;
    CONTACT_VELOCITY_BUCKET velocity = AmtContactEvaluateVelocity(
        rawX, rawY, Contact->HystX, Contact->HystY, dtTicks, PerfFrequencyHz,
        fastThreshold, slowThreshold, alphaFastThreshold,
        PointerRuntime,
        (BOOLEAN)!gestureActive,
        &alphaNum);

    INT deadzoneThreshold = AmtContactDeadzoneForVelocity(velocity, gestureActive, PointerConfig, ScrollConfig);

    // Only the true first sample of a *non*-retap-seeded contact skips the
    // deadzone check (baseline is seeded here, nothing to compare against
    // yet). Every other case - a fresh sample, or a retap-seeded first
    // sample whose HystX/Y baseline already holds the seeded position -
    // runs the same AmtContactEvaluateDeadzone call, so it's kept as one
    // call site instead of being duplicated across two branches.
    if (Contact->PendingFirstSample && !retapSeededFirstSample) {
        Contact->HystX = rawX;
        Contact->HystY = rawY;
        passed = TRUE;
    } else {
        // Keep HystX/Y as-is on the retap-seeded path - they hold the
        // seeded baseline.
        passed = AmtContactEvaluateDeadzone(Contact, rawX, rawY, deadzoneThreshold);
    }

    AmtContactCommitSample(Contact, rawX, rawY, passed, aliveCountIsOne,
                           gestureActive, velocity,
                           alphaNum, retapSeededFirstSample,
                           PointerConfig, ScrollConfig,
                           PointerRuntime, ScrollRuntime,
                           OutX, OutY);

    Contact->LastSlotHint = slotHint;
    Contact->LastSeenQpc  = nowQpc;
    Contact->LastMajor    = major;
    Contact->LastMinor    = minor;
    Contact->LastPressure = pressure;

    if (Contact->FramesAlive < 255)
        Contact->FramesAlive++;
}

#if DBG
VOID
AmtContactPoolCheckInvariants(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool)
{
    for (size_t i = 0; i < MAX_CONTACTS; i++) {
        const ACTIVE_CONTACT* c = &Pool[i];

        if (c->State == CONTACT_FREE) {
            NT_ASSERT(!c->PendingFirstSample);
            NT_ASSERT(!c->RetapSeeded);
            NT_ASSERT(!c->WasInGesture);
            NT_ASSERT(c->ContactID == 0);
            continue;
        }

        NT_ASSERT(c->ContactID != 0);

        for (size_t j = i + 1; j < MAX_CONTACTS; j++) {
            const ACTIVE_CONTACT* d = &Pool[j];
            if (d->State == CONTACT_FREE) continue;
            NT_ASSERT(c->ContactID != d->ContactID);
        }
    }
}
#endif