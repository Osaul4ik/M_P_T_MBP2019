// ActiveContact.c - Contact lifecycle FSM.

#include "Driver.h"
#include "ActiveContact.h"

// AUDIT (2-finger scroll quality, historical): a fixed-threshold deadzone,
// evaluated independently per contact every frame, was also gating genuine
// simultaneous 2(+)-finger movement (scroll). Two fingers scrolling
// together rarely cross a fixed threshold in perfect lockstep frame to
// frame - one finger's delta clears it while the other's doesn't,
// producing an asymmetric stale-vs-updated report between the two
// contacts and a visibly steppy/uneven scroll at slow, deliberate
// speeds. The original fix introduced a second, lower-but-nonzero
// threshold used only while >=2 fingers are concurrently down. That
// approach was superseded by velocity-adaptive filtering below, which
// solves the same problem more directly (a genuinely stationary hold is
// filtered because it's slow, not because it's solo-vs-gesture) and
// also fixes the click/drag-precision case the old fixed threshold
// couldn't reach without going all the way to zero.
#define XY_DEADZONE_UNITS          1  // fallback: VELOCITY_UNKNOWN/MEDIUM
#define XY_DEADZONE_UNITS_SLOW     2  // near-stationary: filter sensor noise
#define XY_DEADZONE_UNITS_FAST     0  // fast motion: no deadzone lag

// Velocity thresholds (normalized device units/sec, Chebyshev distance -
// see AmtContactClassifyVelocity) that select the deadzone threshold and
// solo-movement EMA alpha below. These are an initial estimate based on
// the existing coordinate-range constants elsewhere in this file (e.g.
// RETAP_MAX_DISTANCE), NOT calibrated against real hardware motion
// traces - revisit if real-device testing shows the buckets are mistimed.
#define VELOCITY_SLOW_UNITS_PER_SEC   50
#define VELOCITY_FAST_UNITS_PER_SEC   400

typedef enum _CONTACT_VELOCITY_BUCKET
{
    VELOCITY_UNKNOWN = 0,  // no reliable previous sample/timestamp to
                            // measure against - falls back to the
                            // original fixed threshold/alpha, unchanged
                            // from behavior before this file's adaptive
                            // filtering was added.
    VELOCITY_SLOW,
    VELOCITY_MEDIUM,
    VELOCITY_FAST,
} CONTACT_VELOCITY_BUCKET;

#define SMOOTHING_ALPHA_NUM  3
#define SMOOTHING_ALPHA_DEN  8
// AUDIT (adaptive EMA rework - see AmtContactCommitSample): this used to
// be 2 (25% raw / 75% previous report) - heavy enough that even slow,
// deliberate cursor movement visibly lagged/sprang behind the finger
// ("jelly cursor"), which is exactly the speed regime this bucket now
// governs (EMA is skipped entirely at MEDIUM/FAST - see below). Raised to
// 6 (75% raw): still pulls in enough of the previous report to smooth out
// sub-deadzone-adjacent sensor tremor during fine, near-stationary
// pointing, but no longer produces perceptible trailing lag.
#define SMOOTHING_ALPHA_NUM_SLOW  6  // light smoothing only, near-stationary

// Scroll delta scale: reports ~70% of the raw per-frame delta during
// gestureActive frames (SMOOTHING_ALPHA_* has nothing to do with this -
// EMA is skipped entirely on gesture frames, see AUDIT comment below).
// NOT a resurrection of EMA lag: this scales the delta between
// Contact->ReportX/Y and the incoming raw sample once, per frame, then
// commits the scaled result as the new ReportX/Y baseline - it never
// blends toward a stale previous value or "catches up" over multiple
// frames, so it does not reintroduce the brake-before-stop artifact the
// EMA skip above was written to fix.
#define SCROLL_SCALE_NUM  8
#define SCROLL_SCALE_DEN  10

// Slow-scroll variant: applied instead of SCROLL_SCALE_NUM/DEN above ONLY
// once a gesture's velocity has been CONSISTENTLY slow for several
// frames in a row - see ScrollSlowStreak/ScrollScaleSlow in
// ActiveContact.h and the AUDIT FIX comment on AmtContactCommitSample's
// scroll branch below for why this is debounced instead of switching on
// the raw per-frame VELOCITY_SLOW classification directly. Fast/medium
// 2-finger scroll is deliberately left at SCROLL_SCALE_NUM/DEN (8/10):
// the complaint this fixes is specifically that slow, deliberate
// scrolling felt faster than the finger motion suggested, not that
// scrolling in general is too fast. 14/25 = 0.56, i.e. 0.8 * 0.7 - ~30%
// slower than the existing 0.8x factor, applied only once genuinely slow.
#define SCROLL_SCALE_NUM_SLOW  14
#define SCROLL_SCALE_DEN_SLOW  25

// Consecutive VELOCITY_SLOW gesture frames required before the scroll
// scale switches down to SCROLL_SCALE_*_SLOW. Exiting back to the normal
// rate is NOT debounced - a single non-slow frame exits immediately (see
// AmtContactCommitSample) - because the goal is only to avoid reacting
// to single-frame sensor noise/jitter on the way IN to a genuinely slow,
// deliberate scroll; snapping back to normal speed the instant the user
// actually speeds up should feel immediate, not laggy.
#define SCROLL_SLOW_ENTER_STREAK  3

static inline USHORT
AmtContactSmoothCoord(_In_ USHORT rawVal, _In_ USHORT prevVal, _In_ INT alphaNum)
{
    // rawVal/prevVal are USHORT (>=0) and alphaNum is one of the positive
    // SMOOTHING_ALPHA_NUM_* constants (always < SMOOTHING_ALPHA_DEN), so
    // blended is always >= 0 - no lower-bound clamp needed.
    INT blended = ((INT)rawVal * alphaNum +
                   (INT)prevVal * (SMOOTHING_ALPHA_DEN - alphaNum)) /
                  SMOOTHING_ALPHA_DEN;
    return (USHORT)blended;
}

// Classifies how fast a contact is moving, in normalized device units/sec,
// by comparing the incoming raw sample against the contact's last
// committed Hyst baseline (the same baseline AmtContactEvaluateDeadzone
// tests against) over the elapsed QPC time since the contact was last
// updated. Distance is Chebyshev (max(|dx|,|dy|)) to match the per-axis
// OR-based deadzone test below rather than adding a sqrt for Euclidean
// distance, which kernel mode code avoids (no FPU without explicit
// save/restore).
//
// Returns VELOCITY_UNKNOWN - and callers should treat that as "use the
// original fixed threshold/alpha" - whenever elapsed time or the clock
// frequency isn't usable: DtQpcTicks <= 0 covers both a genuinely
// nonpositive/zero delta AND the deliberate 0 a caller passes for "no
// previous timestamp" (first sample after birth, where LastSeenQpc == 0).
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

    // unitsPerSec = distance * PerfFrequencyHz / DtQpcTicks, multiplying
    // before dividing for integer precision. distance is USHORT-range and
    // PerfFrequencyHz is a QPC frequency (platform-typical range, well
    // under LONGLONG headroom), so this doesn't overflow for realistic
    // values.
    LONGLONG unitsPerSec = ((LONGLONG)distance * PerfFrequencyHz) / DtQpcTicks;

    if (unitsPerSec <= VELOCITY_SLOW_UNITS_PER_SEC) return VELOCITY_SLOW;
    if (unitsPerSec >= VELOCITY_FAST_UNITS_PER_SEC) return VELOCITY_FAST;
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

// AUDIT: FAST's own alpha branch is gone (was SMOOTHING_ALPHA_NUM_FAST) -
// AmtContactCommitSample now only ever runs the EMA blend at VELOCITY_SLOW
// (ordinary movement) or VELOCITY_UNKNOWN (the retap-seeded first sample,
// which is always classified UNKNOWN - see AmtContactClassifyVelocity's
// DtQpcTicks<=0 guard). MEDIUM/FAST movement always skips EMA and reports
// raw, so an alpha value for them would never be read - this function
// still classifies them (for the default case, same value as UNKNOWN) in
// case a future caller needs it, but there's no dedicated FAST constant
// to keep in sync anymore.
static inline INT
AmtContactAlphaForVelocity(_In_ CONTACT_VELOCITY_BUCKET Velocity)
{
    switch (Velocity) {
    case VELOCITY_SLOW: return SMOOTHING_ALPHA_NUM_SLOW;
    case VELOCITY_MEDIUM:
    case VELOCITY_FAST:
    case VELOCITY_UNKNOWN:
    default:             return SMOOTHING_ALPHA_NUM;
    }
}

// Scales one axis of a gesture-frame delta by ScaleNum/ScaleDen (caller
// picks SCROLL_SCALE_NUM/DEN or the _SLOW variant per-frame, see
// AmtContactCommitSample) using a carried remainder (Bresenham-style
// error term), so slow, deliberate scroll motion (e.g. dx=1 unit/frame)
// still moves the cursor instead of being zeroed out by integer
// truncation (1 * 7 / 10 == 0 without this). *Rem must be reset to 0 by
// the caller whenever gestureActive is FALSE. *Rem is carried in the
// units of whichever ScaleDen was in effect when it was last written; a
// frame-to-frame switch between the normal and _SLOW scale (velocity
// crossing the SLOW threshold mid-scroll) reuses the stale remainder
// as-is against the new denominator rather than rescaling it, which can
// bias the very next frame's rounding by at most one unit - negligible
// and self-correcting within a couple of frames, not worth the extra
// bookkeeping to avoid.
static inline SHORT
AmtScaleScrollDelta(_In_ INT rawDelta, _Inout_ LONG* Rem, _In_ INT ScaleNum, _In_ INT ScaleDen)
{
    INT numerator = rawDelta * ScaleNum + *Rem;
    INT scaled    = numerator / ScaleDen;       // truncates toward 0
    *Rem = numerator - scaled * ScaleDen;        // exact remainder
    return (SHORT)scaled;
}

// ContactID issuance: pre-increment, 0 reserved, never reused while warm.
static inline ULONG
AmtContactAssignId(_Inout_ ULONG* NextContactId)
{
    ULONG id = ++(*NextContactId);
    if (id == 0) {
        // Wrapped around ULONG_MAX -> 0. 0 is reserved, so skip it - this
        // keeps the "never reused while warm" invariant true even in the
        // (practically unreachable) case of billions of births with no
        // D0Entry reset in between.
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
    c->RetapSeeded        = FALSE; // plain birth - no seeded baseline to preserve
    c->ScrollRemX          = 0;
    c->ScrollRemY          = 0;
    c->ScrollSlowStreak    = 0;
    c->ScrollScaleSlow     = FALSE;
    c->LastDeltaX          = 0;
    c->LastDeltaY          = 0;
    c->LastSlotHint        = slotHint;
    c->LastSeenQpc         = 0; // set by first AmtContactUpdate call
    c->LastMajor           = 0;
    c->LastMinor           = 0;
    c->LastPressure        = 0;
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
    c->ScrollSlowStreak    = 0;
    c->ScrollScaleSlow     = FALSE;
    c->LastDeltaX          = 0;
    c->LastDeltaY          = 0;
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

// RetapSeeded birth: run EMA normally against seeded baseline. Ordinary
// solo movement also now runs EMA, but ONLY while VelocityIsSlow - see the
// AUDIT comment on skipEma below for why.
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

        // AUDIT (scroll speed + end-of-inertia glitch, generalized to solo
        // pointer movement - "jelly cursor" fix, then made adaptive again):
        // EMA smoothing (SMOOTHING_ALPHA_NUM*/DEN) blends a fraction of raw
        // with the previous report every frame. Running it on EVERY solo
        // movement frame (the original design) made the cursor feel
        // laggy/springy ("jelly"): the reported position exponentially
        // chases the real finger position instead of tracking it, most
        // noticeably during ordinary slow/medium deliberate movement. The
        // fix isn't to delete EMA outright, though - it's still the right
        // tool for smoothing genuine sensor tremor during slow, fine
        // pointing (placing the cursor precisely), where a *little* lag is
        // imperceptible because the finger itself is barely moving, and
        // the alternative (raw) can look shaky at that speed. It's simply
        // the wrong tool once the finger is moving at any real speed - at
        // MEDIUM/FAST the lag becomes very perceptible while the raw
        // signal is already smooth enough (fast motion naturally averages
        // out single-sample sensor noise), so those speeds skip straight
        // to raw. This is the same trade the 2+-finger scroll fix below
        // made permanently (raw always, since scroll speed varies too
        // widely for one alpha to suit) - solo movement instead varies the
        // decision per frame by measured velocity, which is what
        // "adaptive" means here in practice: smoothing exactly where it
        // helps (slow/near-stationary), none where it would just add lag
        // (medium/fast).
        //
        // EMA also always applies for the retap-seeded first sample
        // (commitIsRetapSeededFirstSample) regardless of velocity -
        // AmtContactBirthWithRetapSmoothing deliberately seeds ReportX/Y to
        // the OLD lift position so a slightly-imprecise re-tap eases toward
        // the new position instead of jumping the cursor; that transition
        // is always classified VELOCITY_UNKNOWN (no prior timestamp yet -
        // see AmtContactClassifyVelocity), so it needs its own explicit
        // carve-out here rather than falling out of the velocity check.
        BOOLEAN skipEma =
            gestureActive ||
            (!commitIsRetapSeededFirstSample && !velocityIsSlow);

        if (skipEma) {
            if (gestureActive) {
                // Scroll frame: report ReportX/Y + a fraction of the raw
                // delta, not the raw position itself. Baseline for the
                // *next* frame's delta is this scaled result
                // (Contact->ReportX/Y below), so scaling never compounds
                // across frames - only the current frame's motion is
                // reduced.
                //
                // AUDIT FIX (scale switching on raw per-frame velocity
                // caused jerks, both at slow AND fast overall scroll
                // speed): this originally switched straight off
                // velocityIsSlow (the same AmtContactClassifyVelocity
                // bucket used for solo-pointer deadzone/EMA). That bucket
                // is a single-frame, unsmoothed instantaneous measurement
                // - it already flickers between SLOW/MEDIUM/FAST on
                // ordinary sensor sampling jitter even during objectively
                // steady motion, which deadzone tolerates fine (it only
                // ever adds a LITTLE extra filtering at SLOW). Reusing it
                // to flip the scroll SCALE too compounds that flicker
                // into a visible speed jump every time the bucket
                // flickers - including transiently during otherwise-fast
                // flicks (e.g. the accel/decel edges), which is why fast
                // scrolling got jerky too, not just slow. Fixed by
                // decoupling scroll-scale selection from the raw bucket
                // entirely: ScrollScaleSlow (ActiveContact.h) only
                // switches to the slow rate after SCROLL_SLOW_ENTER_STREAK
                // consecutive VELOCITY_SLOW frames (debounced entry -
                // ordinary sensor jitter can't fake a multi-frame streak),
                // and switches back to the normal rate on the very next
                // non-slow frame (undebounced exit - speeding back up
                // should feel instant, not laggy). Deadzone itself is
                // completely untouched by this - still driven by the raw
                // per-frame bucket exactly as before.
                if (velocityIsSlow) {
                    if (Contact->ScrollSlowStreak < 255) Contact->ScrollSlowStreak++;
                    if (Contact->ScrollSlowStreak >= SCROLL_SLOW_ENTER_STREAK) {
                        Contact->ScrollScaleSlow = TRUE;
                    }
                } else {
                    Contact->ScrollSlowStreak = 0;
                    Contact->ScrollScaleSlow  = FALSE;
                }

                INT dx = (INT)candX - (INT)Contact->ReportX;
                INT dy = (INT)candY - (INT)Contact->ReportY;

                INT scrollScaleNum = Contact->ScrollScaleSlow ? SCROLL_SCALE_NUM_SLOW : SCROLL_SCALE_NUM;
                INT scrollScaleDen = Contact->ScrollScaleSlow ? SCROLL_SCALE_DEN_SLOW : SCROLL_SCALE_DEN;

                LONG newX = (LONG)Contact->ReportX + AmtScaleScrollDelta(dx, &Contact->ScrollRemX, scrollScaleNum, scrollScaleDen);
                LONG newY = (LONG)Contact->ReportY + AmtScaleScrollDelta(dy, &Contact->ScrollRemY, scrollScaleNum, scrollScaleDen);

                // Clamp both ends before truncating to USHORT. The upper
                // clamp can't be hit today (ReportX/Y plus the scaled
                // scroll delta never reach 65535 given current coordinate
                // ranges and MATCH_MAX_CONTINUATION_DELTA), but nothing
                // enforced that - without this, a future change to either
                // constant could silently wrap the reported position to a
                // small value and teleport the cursor.
                repX = (USHORT)(newX < 0 ? 0 : (newX > 0xFFFF ? 0xFFFF : newX));
                repY = (USHORT)(newY < 0 ? 0 : (newY > 0xFFFF ? 0xFFFF : newY));
            } else {
                // Not a scroll frame - this is ordinary movement at
                // MEDIUM/FAST velocity (see skipEma above; SLOW instead
                // falls to the EMA branch below), plus any genuinely-fresh
                // first sample. Report the raw deadzone-committed position
                // directly, and drop any leftover scroll remainder/streak
                // so nothing can leak into a later, unrelated scroll
                // gesture.
                repX = candX;
                repY = candY;

                Contact->ScrollRemX       = 0;
                Contact->ScrollRemY       = 0;
                Contact->ScrollSlowStreak = 0;
                Contact->ScrollScaleSlow  = FALSE;
            }

            if (Contact->WasInGesture && aliveCountIsOne) {
                Contact->WasInGesture = FALSE;
            }
        } else {
            Contact->ScrollRemX       = 0;
            Contact->ScrollRemY       = 0;
            Contact->ScrollSlowStreak = 0;
            Contact->ScrollScaleSlow  = FALSE;
            repX = AmtContactSmoothCoord(candX, Contact->ReportX, alphaNum);
            repY = AmtContactSmoothCoord(candY, Contact->ReportY, alphaNum);
        }
    }

    // Record this frame's committed delta for PTPCore's gesture-last-
    // finger kill deferral (see LastDeltaX/Y doc comment in
    // ActiveContact.h) - MUST be computed against the OLD Contact->
    // ReportX/Y, before it's overwritten below. INT math avoids USHORT
    // wraparound on the subtraction; the result is clamped to SHORT
    // range purely as a defensive bound (MATCH_MAX_CONTINUATION_DELTA
    // already keeps realistic per-frame deltas far inside it).
    INT deltaX = (INT)repX - (INT)Contact->ReportX;
    INT deltaY = (INT)repY - (INT)Contact->ReportY;
    Contact->LastDeltaX = (SHORT)(deltaX < -32768 ? -32768 : (deltaX > 32767 ? 32767 : deltaX));
    Contact->LastDeltaY = (SHORT)(deltaY < -32768 ? -32768 : (deltaY > 32767 ? 32767 : deltaY));

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

    // LastSeenQpc is still last frame's value here - AmtContactUpdate
    // overwrites it further down, after this read. 0 means "no previous
    // sample" (fresh birth, retap-seeded or not) and is handled by
    // AmtContactClassifyVelocity via the DtQpcTicks<=0 guard below.
    LONGLONG prevQpc  = Contact->LastSeenQpc;
    LONGLONG dtTicks  = (prevQpc == 0) ? 0 : (nowQpc - prevQpc);
    CONTACT_VELOCITY_BUCKET velocity = AmtContactClassifyVelocity(
        rawX, rawY, Contact->HystX, Contact->HystY, dtTicks, PerfFrequencyHz);

    // AUDIT FIX (steppy/jerky scroll, both slow and fast): the deadzone
    // threshold below used to apply unconditionally, including on
    // gestureActive (2-finger scroll) frames. Deadzone exists to filter
    // SENSOR NOISE during ordinary single-finger pointing - it's not
    // needed for scroll, which is already gated on genuine, corroborated
    // 2-finger correspondence (Match.c) and has its own dedicated
    // smoothing/speed control (SCROLL_SCALE_NUM/DEN and the debounced
    // _SLOW variant, in AmtContactCommitSample below). Stacking a
    // nonzero deadzone (1 unit at MEDIUM, 2 at SLOW - only FAST was ever
    // 0) on top of that meant any scroll frame not classified FAST held
    // its position outright whenever the raw per-frame delta fell under
    // the threshold, then reported the accumulated delta all at once the
    // moment it finally cleared - a visible hold-then-catch-up stutter.
    // Real 2-finger scroll rarely reaches the FAST bucket (its 400
    // units/sec threshold is tuned for cursor movement), so this hit
    // ordinary scrolling far more than the "fast motion" carve-out
    // suggested. Bypassing deadzone entirely while gestureActive - always
    // report the real raw per-frame position, same as FAST already did -
    // removes the stutter and leaves solo-pointer deadzone completely
    // unchanged.
    INT deadzoneThreshold = gestureActive ? 0 : AmtContactDeadzoneForVelocity(velocity);
    INT alphaNum          = AmtContactAlphaForVelocity(velocity);

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
