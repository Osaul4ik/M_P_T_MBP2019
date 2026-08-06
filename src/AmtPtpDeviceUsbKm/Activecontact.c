// ActiveContact.c - Contact lifecycle FSM.

#include "Driver.h"
#include "ActiveContact.h"

// Adaptive deadzone reduces steppy two-finger scrolling.
#define XY_DEADZONE_UNITS          1  // fallback: VELOCITY_UNKNOWN/MEDIUM
#define XY_DEADZONE_UNITS_SLOW     2  // near-stationary: filter sensor noise
#define XY_DEADZONE_UNITS_FAST     0  // fast motion: no deadzone lag

// Velocity buckets choose deadzone and smoothing.
#define VELOCITY_SLOW_UNITS_PER_SEC   50
#define VELOCITY_FAST_UNITS_PER_SEC   400

typedef enum _CONTACT_VELOCITY_BUCKET
{
    VELOCITY_UNKNOWN = 0,  // no prior timestamp
    VELOCITY_SLOW,
    VELOCITY_MEDIUM,
    VELOCITY_FAST,
} CONTACT_VELOCITY_BUCKET;

#define SMOOTHING_ALPHA_DEN  8
// Lower smoothing reduces slow-speed jitter.
#define SMOOTHING_ALPHA_NUM_SLOW  3  // stronger smoothing, near-stationary

// Continuous alpha ramp endpoints (see AmtContactContinuousAlpha below).
#define ALPHA_NUM_MIN  SMOOTHING_ALPHA_NUM_SLOW  // heaviest blend, near-stationary
#define ALPHA_NUM_MAX  SMOOTHING_ALPHA_DEN       // = raw passthrough, bit-identical

// Scale scroll deltas to reduce abrupt stops.
#define SCROLL_SCALE_NUM  8
#define SCROLL_SCALE_DEN  10

static inline USHORT
AmtContactSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal, _In_ INT alphaNum)
{
    // Blend the new sample with the previous report.
    INT blended = ((INT)rawVal * alphaNum +
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - alphaNum)) /
                  SMOOTHING_ALPHA_DEN;
    return (USHORT)blended;
}

// Estimate motion speed from the last committed sample.
static CONTACT_VELOCITY_BUCKET
AmtContactClassifyVelocity(
    _In_ USHORT   rawX,
    _In_ USHORT   rawY,
    _In_ USHORT   prevX,
    _In_ USHORT   prevY,
    _In_ LONGLONG DtQpcTicks,
    _In_ LONGLONG PerfFrequencyHz
)
{
    if (DtQpcTicks <= 0 || PerfFrequencyHz <= 0) {
        return VELOCITY_UNKNOWN;
    }

    INT dx = (INT)rawX - (INT)prevX;
    if (dx < 0) dx = -dx;
    INT dy = (INT)rawY - (INT)prevY;
    if (dy < 0) dy = -dy;
    INT distance = (dx > dy) ? dx : dy;

    // MICRO-OPT: avoid the 64-bit idiv entirely. The caller only needs the
    // bucket (SLOW/MEDIUM/FAST), never the actual unitsPerSec value, so
    // instead of computing floor(distance*Freq / DtQpcTicks) and comparing
    // it to the two thresholds, cross-multiply and compare the products -
    // same distance, Freq, DtQpcTicks are all non-negative (checked above),
    // so this is an exact match to the original truncating-division
    // comparisons, not an approximation:
    //   floor(N/D) <= c   <=>   N <  (c+1)*D
    //   floor(N/D) >= c   <=>   N >= c*D
    LONGLONG numerator = (LONGLONG)distance * PerfFrequencyHz;

    if (numerator < (LONGLONG)(VELOCITY_SLOW_UNITS_PER_SEC + 1) * DtQpcTicks)
        return VELOCITY_SLOW;
    if (numerator >= (LONGLONG)VELOCITY_FAST_UNITS_PER_SEC * DtQpcTicks)
        return VELOCITY_FAST;
    return VELOCITY_MEDIUM;
}

static inline INT
AmtContactDeadzoneForVelocity(_In_ CONTACT_VELOCITY_BUCKET Velocity)
{
    switch (Velocity) {
    case VELOCITY_SLOW: return XY_DEADZONE_UNITS_SLOW;
    case VELOCITY_FAST: return XY_DEADZONE_UNITS_FAST;
    case VELOCITY_MEDIUM:
    case VELOCITY_UNKNOWN:
    default:             return XY_DEADZONE_UNITS;
    }
}

// Continuous replacement for a discrete velocity-bucket alpha lookup.
// Ramps linearly between ALPHA_NUM_MIN (at/below VELOCITY_SLOW_UNITS_PER_SEC)
// and ALPHA_NUM_MAX (at/above VELOCITY_FAST_UNITS_PER_SEC, bit-identical to
// raw), so a speed anywhere in between gets a proportionally lighter blend
// instead of snapping between two fixed states at a single threshold speed.
// Integer-only (one 64-bit divide per contact per interrupt) - safe at any
// IRQL, no FPU state to save.
static inline INT
AmtContactContinuousAlpha(
    _In_ USHORT   rawX,
    _In_ USHORT   rawY,
    _In_ USHORT   prevX,
    _In_ USHORT   prevY,
    _In_ LONGLONG DtQpcTicks,
    _In_ LONGLONG PerfFrequencyHz
)
{
    // No timestamp basis yet (first sample after birth) - same default as
    // the old bucket scheme for a fresh, non-retap-seeded contact: raw.
    if (DtQpcTicks <= 0 || PerfFrequencyHz <= 0) {
        return ALPHA_NUM_MAX;
    }

    INT dx = (INT)rawX - (INT)prevX;
    if (dx < 0) dx = -dx;
    INT dy = (INT)rawY - (INT)prevY;
    if (dy < 0) dy = -dy;
    INT distance = (dx > dy) ? dx : dy;

    LONGLONG unitsPerSec = ((LONGLONG)distance * PerfFrequencyHz) / DtQpcTicks;

    if (unitsPerSec <= VELOCITY_SLOW_UNITS_PER_SEC) {
        return ALPHA_NUM_MIN;
    }
    if (unitsPerSec >= VELOCITY_FAST_UNITS_PER_SEC) {
        return ALPHA_NUM_MAX;
    }

    LONGLONG span   = VELOCITY_FAST_UNITS_PER_SEC - VELOCITY_SLOW_UNITS_PER_SEC;
    LONGLONG offset = unitsPerSec - VELOCITY_SLOW_UNITS_PER_SEC;

    return ALPHA_NUM_MIN +
           (INT)(((ALPHA_NUM_MAX - ALPHA_NUM_MIN) * offset) / span);
}

// Scale scroll deltas with remainder so slow motion still advances.
static inline SHORT
AmtScaleScrollDelta(_In_ INT rawDelta, _Inout_ LONG* Rem)
{
    INT numerator = rawDelta * SCROLL_SCALE_NUM + *Rem;
    INT scaled    = numerator / SCROLL_SCALE_DEN;       // truncates toward 0
    *Rem = numerator - scaled * SCROLL_SCALE_DEN;        // exact remainder
    return (SHORT)scaled;
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

    c->State             = CONTACT_ACTIVE;
    c->ContactID          = AmtContactAssignId(NextContactId);
    c->ReportX            = x;
    c->ReportY            = y;
    c->HystX              = x;
    c->HystY              = y;
    c->WasInGesture       = FALSE;
    c->PendingFirstSample = TRUE;
    c->RetapSeeded        = FALSE; // no seeded baseline
    c->ScrollRemX          = 0;
    c->ScrollRemY          = 0;
    c->LastSlotHint        = slotHint;
    c->LastSeenQpc         = 0; // set by first AmtContactUpdate
    c->LastMajor           = 0;
    c->LastMinor           = 0;
    c->LastPressure        = 0;
    c->FramesAlive         = 1; // birth frame counts as 1
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

    c->State             = CONTACT_ACTIVE;
    c->ContactID          = AmtContactAssignId(NextContactId);
    c->ReportX            = RecentLiftX;
    c->ReportY            = RecentLiftY;
    c->HystX              = RecentLiftX;
    c->HystY              = RecentLiftY;
    c->WasInGesture       = FALSE;
    c->PendingFirstSample = TRUE;
    c->RetapSeeded        = TRUE; // preserve seed on first update
    c->ScrollRemX          = 0;
    c->ScrollRemY          = 0;
    c->LastSlotHint        = slotHint;
    c->LastSeenQpc         = 0;
    c->LastMajor           = 0;
    c->LastMinor           = 0;
    c->LastPressure        = 0;
    c->FramesAlive         = 1;
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

    INT dx = (INT)CandX - (INT)LiftX;
    if (dx < 0) dx = -dx;
    INT dy = (INT)CandY - (INT)LiftY;
    if (dy < 0) dy = -dy;

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

    INT dx = (INT)candX - (INT)Contact->HystX;
    if (dx < 0) dx = -dx;
    INT dy = (INT)candY - (INT)Contact->HystY;
    if (dy < 0) dy = -dy;

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
    _In_    BOOLEAN         velocityIsSlow,
    _In_    INT             alphaNum,
    _In_    BOOLEAN         commitIsRetapSeededFirstSample,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
)
{
    USHORT repX, repY;

    if (!passedDeadzone) {
        repX = Contact->ReportX;
        repY = Contact->ReportY;
    } else {
        Contact->HystX = candX;
        Contact->HystY = candY;

        if (gestureActive) {
            // Scroll frame: report ReportX/Y + 70% of the raw delta, not
            // the raw position itself. Baseline for the next frame's delta
            // is this scaled result, so scaling never compounds across
            // frames.
            INT dx = (INT)candX - (INT)Contact->ReportX;
            INT dy = (INT)candY - (INT)Contact->ReportY;

            LONG newX = (LONG)Contact->ReportX + AmtScaleScrollDelta(dx, &Contact->ScrollRemX);
            LONG newY = (LONG)Contact->ReportY + AmtScaleScrollDelta(dy, &Contact->ScrollRemY);

            // Clamp both ends before truncating to USHORT.
            repX = (USHORT)(newX < 0 ? 0 : (newX > 0xFFFF ? 0xFFFF : newX));
            repY = (USHORT)(newY < 0 ? 0 : (newY > 0xFFFF ? 0xFFFF : newY));
        } else {
            // Retap-seeded first sample always eases in from the seed
            // (no real dt to measure velocity from yet - see
            // AmtContactUpdate). Everything else blends by the continuous
            // alpha the caller computed from measured velocity: heaviest
            // smoothing near-stationary, ramping linearly to a
            // bit-identical raw passthrough by VELOCITY_FAST_UNITS_PER_SEC.
            // Replaces the old hard SLOW/MEDIUM/FAST switch, which only
            // ever reported a fixed 3/8 blend or exactly raw with nothing
            // between - a deliberate slow move sitting right at the
            // threshold speed could flip frame-to-frame between the two.
            INT effAlpha = commitIsRetapSeededFirstSample ? ALPHA_NUM_MIN : alphaNum;

            repX = AmtContactSmoothCoord(candX, Contact->ReportX, effAlpha);
            repY = AmtContactSmoothCoord(candY, Contact->ReportY, effAlpha);

            Contact->ScrollRemX = 0;
            Contact->ScrollRemY = 0;
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
    CONTACT_VELOCITY_BUCKET velocity = AmtContactClassifyVelocity(
        rawX, rawY, Contact->HystX, Contact->HystY, dtTicks, PerfFrequencyHz);

    INT deadzoneThreshold = AmtContactDeadzoneForVelocity(velocity);
    INT alphaNum          = AmtContactContinuousAlpha(
        rawX, rawY, Contact->HystX, Contact->HystY, dtTicks, PerfFrequencyHz);

    if (Contact->PendingFirstSample) {
        if (retapSeededFirstSample) {
            // Keep HystX/Y - they hold the seeded baseline.
            passed = AmtContactEvaluateDeadzone(Contact, rawX, rawY, deadzoneThreshold);
        } else {
            Contact->HystX = rawX;
            Contact->HystY = rawY;
            passed = TRUE;
        }
    } else {
        passed = AmtContactEvaluateDeadzone(Contact, rawX, rawY, deadzoneThreshold);
    }

    AmtContactCommitSample(Contact, rawX, rawY, passed, aliveCountIsOne,
                           gestureActive, (BOOLEAN)(velocity == VELOCITY_SLOW),
                           alphaNum, retapSeededFirstSample,
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