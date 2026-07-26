// ActiveContact.c - Contact lifecycle FSM.

#include "Driver.h"
#include "ActiveContact.h"

#define XY_DEADZONE_UNITS          1  // solo movement: click/drag precision
// AUDIT (2-finger scroll quality): the same 2-unit threshold, evaluated
// independently per contact every frame, was also gating genuine
// simultaneous 2(+)-finger movement (scroll). Two fingers scrolling
// together rarely cross a fixed threshold in perfect lockstep frame to
// frame - one finger's delta clears it while the other's doesn't,
// producing an asymmetric stale-vs-updated report between the two
// contacts and a visibly steppy/uneven scroll at slow, deliberate
// speeds. A lower (not zero) threshold specifically while >=2 fingers
// are concurrently down keeps genuinely stationary multi-finger holds
// (e.g. a 2-finger tap-and-hold for right-click) still filtered against
// single-unit sensor noise, while roughly doubling responsiveness for
// real scroll motion. This only changes raw contact position fidelity -
// Windows' own PTP stack still derives the scroll gesture itself from
// these contacts, so this stays within the PTP contract.
#define XY_DEADZONE_UNITS_GESTURE   1
#define SMOOTHING_ALPHA_NUM  5
#define SMOOTHING_ALPHA_DEN  8

// Scroll delta scale: reports ~70% of the raw per-frame delta during
// gestureActive frames (SMOOTHING_ALPHA_* has nothing to do with this -
// EMA is skipped entirely on gesture frames, see AUDIT comment below).
// NOT a resurrection of EMA lag: this scales the delta between
// Contact->ReportX/Y and the incoming raw sample once, per frame, then
// commits the scaled result as the new ReportX/Y baseline - it never
// blends toward a stale previous value or "catches up" over multiple
// frames, so it does not reintroduce the brake-before-stop artifact the
// EMA skip above was written to fix.
#define SCROLL_SCALE_NUM  7
#define SCROLL_SCALE_DEN  10

static inline USHORT
AmtContactSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal)
{
    INT blended = ((INT)rawVal * SMOOTHING_ALPHA_NUM +
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - SMOOTHING_ALPHA_NUM)) /
                  SMOOTHING_ALPHA_DEN;
    return (USHORT)(blended < 0 ? 0 : blended);
}

// Scales one axis of a gesture-frame delta by SCROLL_SCALE_NUM/DEN using a
// carried remainder (Bresenham-style error term), so slow, deliberate
// scroll motion (e.g. dx=1 unit/frame) still moves the cursor instead of
// being zeroed out by integer truncation (1 * 7 / 10 == 0 without this).
// *Rem must be reset to 0 by the caller whenever gestureActive is FALSE.
static inline SHORT
AmtScaleScrollDelta(_In_ INT rawDelta, _Inout_ LONG* Rem)
{
    INT numerator = rawDelta * SCROLL_SCALE_NUM + *Rem;
    INT scaled    = numerator / SCROLL_SCALE_DEN;       // truncates toward 0
    *Rem = numerator - scaled * SCROLL_SCALE_DEN;        // exact remainder
    return (SHORT)scaled;
}

// ContactID issuance: pre-increment, 0 reserved, never reused while warm.
static inline ULONG
AmtContactAssignId(_Inout_ ULONG* NextContactId)
{
    return ++(*NextContactId);
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
    c->RetapSeeded        = FALSE; // plain birth - no seeded baseline to preserve
    c->ScrollRemX          = 0;
    c->ScrollRemY          = 0;
    c->LastSlotHint        = slotHint;
    c->LastSeenQpc         = 0; // set by first AmtContactUpdate call
    c->FramesAlive         = 1; // birth frame counts as 1
}

// Seeds EMA baseline to lift position so cursor doesn't jump on re-tap.
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
    // Seed EMA baseline with lift position for smooth cursor continuity
    // on subsequent MOVE frames.
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
        return FALSE; // no usable clock - fail closed

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

// Button-click synthetic rebirth (Windows PTP anti-jitter snap workaround).
// Deliberately the narrowest possible state change: only ContactID moves.
// State stays CONTACT_ACTIVE, position/Hyst/EMA baseline/WasInGesture/
// FramesAlive/RetapSeeded/LastSlotHint/LastSeenQpc are all untouched, so
// the contact's smoothing continuity is unaffected by the identity swap.
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

// RetapSeeded birth: run EMA normally against seeded baseline.
static inline VOID
AmtContactCommitSample(
    _Inout_ PACTIVE_CONTACT Contact,
    _In_    USHORT          candX,
    _In_    USHORT          candY,
    _In_    BOOLEAN         passedDeadzone,
    _In_    BOOLEAN         aliveCountIsOne,
    _In_    BOOLEAN         gestureActive,
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

        // AUDIT (scroll speed + end-of-inertia glitch): EMA smoothing
        // (SMOOTHING_ALPHA_NUM/DEN) blends 62.5% raw / 37.5% previous
        // report every frame. During solo pointer movement that's a
        // reasonable jitter filter. During 2(+)-finger movement it does
        // two things Windows' own PTP scroll/inertia math doesn't
        // expect: (1) it permanently damps the reported per-frame delta
        // below the finger's real displacement, which reads as slower
        // scrolling than the physical gesture; (2) after the finger
        // decelerates and stops, the filter keeps asymptotically
        // creeping toward the final raw position for a few more frames
        // even though the finger is no longer moving - Windows samples
        // the trailing frames before lift-off to seed inertia velocity,
        // so this creep shows up as a brief, inconsistent "hitch" right
        // at the transition into momentum scrolling instead of a clean
        // stop. Reporting the true raw (deadzone-committed) position
        // during gesture frames removes both artifacts - Windows' own
        // stack already does its own filtering/velocity estimation on
        // genuine digitizer input, which is what PTP expects it to
        // receive; this doesn't add any scroll logic of our own, only
        // stops hiding the real finger trajectory from it.
        BOOLEAN skipEma =
            (Contact->PendingFirstSample && !commitIsRetapSeededFirstSample) ||
            (Contact->WasInGesture && aliveCountIsOne) ||
            gestureActive;

        if (skipEma) {
            if (gestureActive) {
                // Scroll frame: report ReportX/Y + 70% of the raw delta,
                // not the raw position itself. Baseline for the *next*
                // frame's delta is this scaled result (Contact->ReportX/Y
                // below), so scaling never compounds across frames - only
                // the current frame's motion is reduced, same as the raw
                // path was doing before this change, just at 70% rate.
                INT dx = (INT)candX - (INT)Contact->ReportX;
                INT dy = (INT)candY - (INT)Contact->ReportY;

                LONG newX = (LONG)Contact->ReportX + AmtScaleScrollDelta(dx, &Contact->ScrollRemX);
                LONG newY = (LONG)Contact->ReportY + AmtScaleScrollDelta(dy, &Contact->ScrollRemY);

                repX = (USHORT)(newX < 0 ? 0 : newX);
                repY = (USHORT)(newY < 0 ? 0 : newY);
            } else {
                // Not a scroll frame (first sample, or solo frame right
                // after a gesture ends) - report the raw position as
                // before, and drop any leftover scroll remainder so it
                // can't leak into a later, unrelated scroll gesture.
                repX = candX;
                repY = candY;

                Contact->ScrollRemX = 0;
                Contact->ScrollRemY = 0;
            }

            if (Contact->WasInGesture && aliveCountIsOne) {
                Contact->WasInGesture = FALSE;
            }
        } else {
            Contact->ScrollRemX = 0;
            Contact->ScrollRemY = 0;
            repX = AmtContactSmoothCoord(candX, Contact->ReportX);
            repY = AmtContactSmoothCoord(candY, Contact->ReportY);
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
    _In_    USHORT          slotHint,
    _In_    LONGLONG        nowQpc,
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

    INT deadzoneThreshold =
        gestureActive ? XY_DEADZONE_UNITS_GESTURE : XY_DEADZONE_UNITS;

    if (Contact->PendingFirstSample) {
        if (retapSeededFirstSample) {
            // Do not reset HystX/Y - they hold the seeded baseline.
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
                           gestureActive, retapSeededFirstSample, OutX, OutY);

    Contact->LastSlotHint = slotHint;
    Contact->LastSeenQpc  = nowQpc;

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
