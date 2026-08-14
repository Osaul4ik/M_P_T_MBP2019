// Contact lifecycle and pool state for active touch tracking.
// Pool slots are separate from hardware slots.

#pragma once

#include "public.h"
#include <Hid.h>

EXTERN_C_START

// Precomputed fixed-point coefficients used only by the input hot path.
// User-facing percentages remain in AMT_*_CONFIG for IOCTL/registry compatibility,
// but the interrupt path never performs percent division.
#define AMT_RUNTIME_FIXED_SHIFT 32
#define AMT_RUNTIME_FIXED_ONE   (1LL << AMT_RUNTIME_FIXED_SHIFT)

typedef struct _AMT_POINTER_RUNTIME
{
    LONGLONG CursorSpeedQ32;
    INT  CursorSmoothingAlphaNum;
    INT  CursorSmoothingAlphaNumSlow;
    INT  CursorSmoothingAlphaDen;
    LONGLONG CursorSmoothingSlopeQ32;
} AMT_POINTER_RUNTIME, *PAMT_POINTER_RUNTIME;

typedef struct _AMT_SCROLL_RUNTIME
{
    LONGLONG BaseScaleQ32;
    LONGLONG FastScaleQ32;
    LONGLONG SmoothingAlphaQ32;
} AMT_SCROLL_RUNTIME, *PAMT_SCROLL_RUNTIME;

typedef enum _CONTACT_STATE
{
    CONTACT_FREE = 0,  // free pool slot, no identity
    CONTACT_ACTIVE,    // finger down, identity live
    CONTACT_GRACE,     // same-frame quarantine after gesture lift
} CONTACT_STATE;

// One pool entry; pool slot is not the identity.
//
// MICRO-OPT: aligned/padded to 64 bytes (Intel L1/L2/L3 cache line size,
// including i9-9750H) so each pool entry occupies exactly one cache line
// instead of straddling two (natural size was 48B, so entries used to
// drift across line boundaries in the 5-entry array).
//
// CAVEAT: this is a compiler-side alignment hint, not a runtime guarantee.
// ACTIVE_CONTACT lives inside DEVICE_CONTEXT, which WDF allocates via its
// own pool allocator (guarantees only MEMORY_ALLOCATION_ALIGNMENT = 16B on
// x64, not 64B). So the array may or may not actually land on 64B-aligned
// addresses at runtime - the compiler emits code assuming it does, but
// nothing enforces it. Harmless either way (x64 tolerates unaligned
// ordinary loads/stores - no crash risk), but treat this as "free if it
// works, no-op if it doesn't", not a guaranteed win.
typedef struct DECLSPEC_ALIGN(64) _ACTIVE_CONTACT
{
    CONTACT_STATE State;

    // Windows-facing identity. Valid while State != CONTACT_FREE.
    // Never reused while warm.
    ULONG ContactID;

    // Reported (post deadzone + EMA) position, normalized device units.
    USHORT ReportX;
    USHORT ReportY;

    // Hysteresis/deadzone baseline. Distinct from ReportX/Y (post-EMA).
    USHORT HystX;
    USHORT HystY;

    // TRUE if contact was in >=2-finger frame during ACTIVE lifetime.
    // Causes EMA skip on first solo frame (aliveCountIsOne).
    BOOLEAN WasInGesture;

    // First-sample flag; skips the initial deadzone/EMA path when needed.
    BOOLEAN PendingFirstSample;

    // Keeps the retap seed from being overwritten on the first sample.
    BOOLEAN RetapSeeded;

    // Carries fractional scroll error across frames within one gesture.
    LONG ScrollRemX;
    LONG ScrollRemY;
    LONG ScrollFilteredX;
    LONG ScrollFilteredY;

    // ---- Matching-hint fields. NOT identity. ----
    USHORT   LastSlotHint;    // hw slot matched to last frame; speeds up matching
    LONGLONG LastSeenQpc;     // QPC of last successful match; grace/retap timing

    // Geometry/pressure, used ONLY as a matching tie-break (Match.c).
    // 0 until the first AmtContactUpdate call.
    USHORT   LastMajor;
    USHORT   LastMinor;
    USHORT   LastPressure;

    // Frames this contact has been alive, saturating at 255. NOT an
    // identity/matching hint - purely a diagnostic/age counter.
    UCHAR FramesAlive;
} ACTIVE_CONTACT, *PACTIVE_CONTACT;

#define MAX_CONTACTS PTP_MAX_CONTACT_POINTS  // pool capacity, not slot count

// Zero/FREE-initialise the whole pool. Call at device creation and D0Entry.
VOID
AmtContactPoolInit(_Out_writes_(MAX_CONTACTS) PACTIVE_CONTACT Pool);

// Finds a FREE pool entry. Returns index or MAX_CONTACTS if full.
size_t
AmtContactPoolFindFree(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool);

// FREE -> ACTIVE. Assigns fresh ContactID, seeds baseline.
// Precondition: Pool[index].State == CONTACT_FREE.
VOID
AmtContactBirth(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          x,
    _In_    USHORT          y,
    _In_    USHORT          slotHint
);

// Rebind an active contact to a new identity without resetting smoothing state.
VOID
AmtContactRebindIdentity(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _Out_   ULONG*          OldContactID
);

// Birth a contact with a retap-based seed.
VOID
AmtContactBirthWithRetapSmoothing(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Inout_ ULONG*          NextContactId,
    _In_    USHORT          RecentLiftX,
    _In_    USHORT          RecentLiftY,
    _In_    USHORT          slotHint
);

// Check whether a down event is near a recent lift.
#define RETAP_WINDOW_100NS      (150LL * 10000LL)  // was 700 ms
#define RETAP_MAX_DISTANCE      200                 // was 600 (normalized units)

BOOLEAN
AmtContactIsRecentLiftNearby(
    _In_ LONGLONG LiftQpc,
    _In_ USHORT   LiftX,
    _In_ USHORT   LiftY,
    _In_ LONGLONG NowQpc,
    _In_ LONGLONG PerfFrequencyHz,
    _In_ USHORT   CandX,
    _In_ USHORT   CandY
);

// Lift a contact and return its last state.
VOID
AmtContactKill(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
);

// Move an active contact into grace state for one frame.
VOID
AmtContactEnterGrace(
    _Inout_ PACTIVE_CONTACT Pool,
    _In_    size_t          index,
    _Out_   ULONG*          OldContactID,
    _Out_   USHORT*         OldX,
    _Out_   USHORT*         OldY
);

// Drop a grace contact and clear its slot.
VOID
AmtContactExpireGrace(_Inout_ PACTIVE_CONTACT Pool, _In_ size_t index);

// Check whether a candidate passes the deadzone test.
BOOLEAN
AmtContactEvaluateDeadzone(
    _In_ const ACTIVE_CONTACT* Contact,
    _In_ USHORT                candX,
    _In_ USHORT                candY,
    _In_ INT                   ThresholdUnits
);

// Update an active contact for one frame using deadzone and EMA.
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
    _In_    const AMT_POINTER_RUNTIME* PointerRuntime,
    _In_    const AMT_SCROLL_RUNTIME* ScrollRuntime,
    _Out_   USHORT*         OutX,
    _Out_   USHORT*         OutY
);

#if DBG
// Debug invariants.
VOID
AmtContactPoolCheckInvariants(_In_reads_(MAX_CONTACTS) const ACTIVE_CONTACT* Pool);
#else
#define AmtContactPoolCheckInvariants(Pool) ((VOID)0)
#endif

EXTERN_C_END