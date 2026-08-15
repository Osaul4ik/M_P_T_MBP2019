// Device.h - Device definitions. Kernel-mode Driver Framework

#include "public.h"
#include <Hid.h>
#include "ActiveContact.h"
#include "PTPCore.h"
#include "Palm.h"

EXTERN_C_START

// Hold ordinary click reports until the press is classified.
typedef enum _CLICK_ARBITRATION_STATE
{
    CLICK_ARBITRATION_IDLE = 0,    // button not down
    CLICK_ARBITRATION_PENDING,     // button down, still deciding
    CLICK_ARBITRATION_HARD_TAP,    // decided: ordinary click - report it
    CLICK_ARBITRATION_FORCE_TOUCH  // decided: force touch - suppress click
} CLICK_ARBITRATION_STATE;

// Synthetic right-click delivery for force-touch clicks.
//
// REWORKED: previously a flat ring of individual Button2 edges (down/up
// treated as interchangeable entries), evicted from the head on overflow.
// That could orphan an already-delivered DOWN when its matching UP got
// evicted before delivery (e.g. several force-touch clicks landing
// back-to-back while mouhid.sys's read cadence lagged) - Windows would
// then see Button2 latched down with no UP ever coming: a stuck
// right-click. Split into two pieces so that failure mode is structurally
// impossible:
//
//  - ForceTouchDeliveryState: the ONE click currently being delivered.
//    Its UP is always guaranteed to be sent (eventually) before a new
//    click's DOWN is ever started - so a DOWN can never reach the host
//    without its UP following.
//  - PendingForceTouchClickCount: clicks still waiting to START. These
//    have delivered nothing yet, so dropping the oldest on overflow (a
//    very fast click storm outrunning available mouse read requests) is
//    always safe - at worst a click gets silently coalesced away, never
//    a stuck button.
typedef enum _FORCE_TOUCH_DELIVERY_STATE
{
    FORCE_TOUCH_DELIVERY_IDLE = 0,     // nothing in flight
    FORCE_TOUCH_DELIVERY_DOWN_PENDING, // DOWN not yet delivered
    FORCE_TOUCH_DELIVERY_UP_PENDING    // DOWN delivered, UP not yet delivered
} FORCE_TOUCH_DELIVERY_STATE;

typedef struct _AMT_CONFIG_CONTROL_CONTEXT
{
    // The PnP filter device that owns the actual palm/geometry state.
    // This is a WDF handle, not a raw WDM pointer.
    WDFDEVICE TargetDevice;
} AMT_CONFIG_CONTROL_CONTEXT, *PAMT_CONFIG_CONTROL_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(
    AMT_CONFIG_CONTROL_CONTEXT,
    AmtConfigControlGetContext
)


typedef struct _DEVICE_CONTEXT
{
    // Protect shared frame-processing state across concurrent USB completions.
    WDFSPINLOCK     StateLock;

    // USB - touched every interrupt/report cycle.
    WDFUSBDEVICE    UsbDevice;
    WDFUSBPIPE      InterruptPipe;
    WDFQUEUE        InputQueue;

    // User-mode configuration endpoint. This is a separate KMDF control
    // device; the PnP FDO remains a lower filter and does not expose the
    // GUI interface directly on the USB/HID stack.
    WDFDEVICE       ConfigControlDevice;

    // Opt-in live monitor state. FALSE is the normal/idle state and the
    // interrupt path does not copy any live-monitor data when it is false.
    BOOLEAN         LiveEnabled;
    ULONG           LiveSequence;
    AMT_LIVE_FRAME  LiveFrame;

    // Device config
    const struct BCM5974_CONFIG* DeviceInfo;
    BOOLEAN IsWellspringModeOn;

    // Runtime-tunable palm-rejection thresholds (see AMT_PALM_CONFIG in
    // Public.h). Initialized to AMT_PALM_CONFIG_DEFAULT_INIT in
    // AmtPtpDeviceUsbKmCreateDevice, then optionally overridden from the
    // registry (AmtPalmConfigLoadFromRegistry) and/or live via
    // IOCTL_AMT_PTP_SET_PALM_CONFIG from AmtPtpConfigGui. Read every frame
    // by AmtPalmClassify (Palm.c) - protected by StateLock on write so a
    // config update from the GUI thread can't race a frame in flight.
    AMT_PALM_CONFIG PalmConfig;
    AMT_PALM_RUNTIME PalmRuntime;

    // Runtime-tunable Force Tap (force touch) threshold and click action
    // (see AMT_POINTER_CONFIG in Public.h). Initialized to
    // AMT_POINTER_CONFIG_DEFAULT_INIT in AmtPtpDeviceUsbKmCreateDevice, then
    // optionally overridden from the registry (AmtPointerConfigLoadFromRegistry)
    // and/or live via IOCTL_AMT_PTP_SET_POINTER_CONFIG from AmtPtpConfigGui.
    // Read every frame by PTPCore_ProcessFrame (Ptpcore.c) for the pressure
    // threshold, and by the force-touch delivery block in Interrupt.c for
    // the action - protected by StateLock on write so a config update from
    // the GUI thread can't race a frame in flight.
    AMT_POINTER_CONFIG PointerConfig;
    AMT_POINTER_RUNTIME PointerRuntime;

    // Runtime-tunable two-finger scroll behavior.
    AMT_SCROLL_CONFIG ScrollConfig;
    AMT_SCROLL_RUNTIME ScrollRuntime;

    // TRUE only for trackpads whose packet format actually carries a
    // pressure reading (TYPE4/TYPE5 - see AppleDefinition.h). Derived once
    // from DeviceInfo->tp_type in EvtDevicePrepareHardware. TYPE1-3
    // trackpads (e.g. WELLSPRING8 / pre-Force-Touch MacBook Air) have no
    // pressure channel at all, so force-touch arbitration is meaningless
    // for them - click handling falls back to a plain mechanical hard tap.
    BOOLEAN SupportsForceTouch;

    // PTP state
    BOOLEAN PtpInputOn;
    BOOLEAN PtpReportTouch;
    BOOLEAN PtpReportButton;

    // Track the prior button state for click-edge detection.
    BOOLEAN PrevButtonClicked;

    // Lock out force-touch if the press drags away from the anchor.
    BOOLEAN ForceTouchAnchorValid;
    USHORT  ForceTouchAnchorX;
    USHORT  ForceTouchAnchorY;
    BOOLEAN ForceTouchDragLockout;

    // Force-touch click delivery state - see FORCE_TOUCH_DELIVERY_STATE
    // above for the full rationale.
#define PENDING_FORCE_TOUCH_CLICK_CAPACITY 8 // queued (not-yet-started) clicks
    FORCE_TOUCH_DELIVERY_STATE ForceTouchDeliveryState;
    UCHAR                       PendingForceTouchClickCount;

    // Click-arbitration state and timing.
    CLICK_ARBITRATION_STATE ClickArbitrationState;
    USHORT                  ClickArbitrationPeakPressure;
    LONGLONG                ClickArbitrationStartQpc;
    // Frames PeakPressure has NOT grown; lets a flat/light press resolve
    // to HARD_TAP before the full safety-net timeout.
    UCHAR                   ClickArbitrationStallFrames;

    // Scan time
    LARGE_INTEGER LastReportTime;

    // Running scan-time counter for the PTP report.
    ULONG ScanTimeAccumulator;

    // Session-level palm latch used by PTPCore.
    BOOLEAN PalmDetected;

    // Contact pool for PTPCore and ActiveContact. Allocated separately
    // (not embedded) and manually aligned to 64 bytes in
    // AmtAllocateAlignedContactPool - WDF's context allocator does not
    // honor DECLSPEC_ALIGN, so an embedded array only inherited whatever
    // alignment the pool allocator happened to give the whole context
    // block. Freed in AmtPtpEvtDeviceContextCleanup.
    // ---------------------------------------------------------------
    PACTIVE_CONTACT ActiveContacts;

    // Monotonic ContactID counter - never reuses an ID while "warm".
    // Every lift-off advances it; reseeded at D0Entry.
    ULONG   NextContactId;

    // QPC frequency cached at D0Entry
    LARGE_INTEGER PerfFrequency;

    // MICRO-OPT: PerfFrequency-derived tick thresholds, computed once at
    // D0Entry instead of on every call site that needs them (retap window
    // check on every birth, match time-reject on every accepted match,
    // click-arbitration timeout on every pending frame). Each of those was
    // a 64-bit multiply+divide recomputed from the same constant frequency
    // every time; now it's a plain field read. 0 means "no usable clock"
    // (PerfFrequency.QuadPart <= 0), preserving the original fail-closed/
    // fail-open behavior of each call site exactly.
    LONGLONG      RetapWindowTicks;             // AmtRecentLiftFindNearby window
    LONGLONG      MatchMaxTimeDeltaTicks;       // AmtMatchCorrespond time-reject
    LONGLONG      ClickArbitrationTimeoutTicks; // click arbitration safety-net

    // MICRO-OPT: Q16 fixed-point form of (10000 / PerfFrequency), computed
    // once at D0Entry instead of a 64-bit divide (and a PerfFrequency>0
    // branch) on every single USB interrupt completion (the hottest
    // routine in the driver - see AmtPtpEvtUsbInterruptPipeReadComplete).
    // Runtime becomes an unconditional multiply + shift:
    // (PerfDelta * ScanTimeScaleQ16) >> 16, same result as the old
    // "* 10000LL / PerfFrequency.QuadPart" to within the ScanTime field's
    // USHORT-truncated precision (rounding differs by at most 1 part in
    // 65536, invisible after the field's own 0xFFFF wraparound). Q16 (not
    // Q32) is deliberate: it keeps PerfDelta*ScanTimeScaleQ16 far from
    // LONGLONG overflow even after a multi-day idle gap between reports,
    // where Q32's much larger scale factor would leave far less headroom.
    // When there's no usable clock (PerfFrequency.QuadPart <= 0, meaning
    // KeQueryPerformanceCounter reported a degenerate frequency - not
    // expected on real hardware), this is set to 655 instead of 0: solving
    // (x*scale)>>16 == x/100 for scale gives 65536/100 = 655.36, matching
    // the old "/100LL" fallback formula while still avoiding a branch on
    // the hot path - the fallback is already a best-effort approximation,
    // so the ~0.05% rounding difference from truncating to 655 is moot.
    LONGLONG      ScanTimeScaleQ16;

    // Deferred overflow queue for contact events.
    // ---------------------------------------------------------------
    ULONG         OverflowContactID[PTP_MAX_CONTACT_POINTS];
    USHORT        OverflowX[PTP_MAX_CONTACT_POINTS];
    USHORT        OverflowY[PTP_MAX_CONTACT_POINTS];
    CONTACT_PHASE OverflowPhase[PTP_MAX_CONTACT_POINTS];
    BOOLEAN       OverflowConfident[PTP_MAX_CONTACT_POINTS];
    UCHAR         OverflowCount;

    // Recent-lift memory for retap smoothing.
    // ---------------------------------------------------------------
    RECENT_LIFT_RING RecentLifts;

    // ---------------------------------------------------------------
    // Cold: USB setup metadata. Written once (EvtDevicePrepareHardware /
    // device creation), never read in the per-frame hot path (grep across
    // Interrupt.c/Hid.c/Ptpcore.c/Match.c confirms zero hits) - kept at the
    // end of the struct so it doesn't sit between hot fields and cost extra
    // cache-line fetches on the frame-processing path.
    // ---------------------------------------------------------------
    WDFUSBINTERFACE        UsbInterface;
    USB_DEVICE_DESCRIPTOR  DeviceDescriptor;
    ULONG                  UsbDeviceTraits;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

// Bound the Wellspring control transfer with a timeout.
#define WELLSPRING_CONTROL_TRANSFER_TIMEOUT_SEC   5

NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    );

EVT_WDF_DEVICE_PREPARE_HARDWARE AmtPtpDeviceUsbKmEvtDevicePrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY         AmtPtpEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT          AmtPtpEvtDeviceD0Exit;
EVT_WDF_OBJECT_CONTEXT_CLEANUP  AmtPtpEvtDeviceContextCleanup;

// The PnP device is a lower filter. User-mode configuration therefore uses
// a separate KMDF control device with a DOS symbolic link instead of a
// device interface attached to the USB/HID filter FDO.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpCreateConfigControlDevice(_In_ WDFDEVICE TargetDevice);

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AmtPtpConfigControlEvtIoDeviceControl;

// Fires when the last handle to \\DosDevices\\AmtPtpDeviceUsbKm closes -
// including an abnormal GUI exit (crash/kill/unplug), where the GUI never
// gets a chance to run its own Closed handler. Used as a safety net to
// force LiveEnabled back off so the interrupt hot path never keeps building
// live snapshots for a monitor that no longer exists.
EVT_WDF_FILE_CLOSE AmtPtpConfigControlEvtFileClose;

NTSTATUS
AmtPtpSetLiveEnabled(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

NTSTATUS
AmtPtpGetLiveFrame(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(_In_ WDFDEVICE Device);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(_In_ PDEVICE_CONTEXT DeviceContext);

EVT_WDF_USB_READER_COMPLETION_ROUTINE AmtPtpEvtUsbInterruptPipeReadComplete;
EVT_WDF_USB_READERS_FAILED            AmtPtpEvtUsbInterruptReadersFailed;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN IsWellspringModeOn
);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetHidDescriptor(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetDeviceAttribs(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpGetReportDescriptor(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

NTSTATUS AmtPtpDispatchReadReportRequests(
    _In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _Out_ BOOLEAN* Pending);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpReportFeatures(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS AmtPtpSetFeatures(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

// ConfigIoctl.c - AmtPtpConfigGui <-> driver custom IOCTL surface.

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpSetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetPadGeometry(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpResetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpSetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpResetPointerConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpSetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpResetScrollConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS AmtPtpGetDeviceInfo(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request);

// Best-effort registry persistence under the device's driver-software key
// (HKLM\SYSTEM\...\Enum\...\Device Parameters, via WdfDeviceOpenRegistryKey).
// Failure to read/write the registry is never fatal - PalmConfig always
// falls back to AMT_PALM_CONFIG_DEFAULT_INIT.
_IRQL_requires_(PASSIVE_LEVEL)
VOID AmtPalmConfigLoadFromRegistry(_In_ WDFDEVICE Device, _Inout_ PAMT_PALM_CONFIG Config);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID AmtPalmConfigSaveToRegistry(_In_ WDFDEVICE Device, _In_ const AMT_PALM_CONFIG* Config);

// Clamps every tunable field of Config in place to the ranges declared in
// Public.h (AMT_PALM_*_MAX). Used on every path that accepts values from
// user mode (SET IOCTL and registry load) so a corrupt registry value or a
// buggy/malicious caller can never push the classifier into a degenerate
// state (e.g. an edge zone covering the whole pad).
VOID AmtPalmConfigClamp(_Inout_ PAMT_PALM_CONFIG Config);

VOID AmtPalmRuntimeRebuild(
    _In_ const AMT_PALM_CONFIG* Config,
    _Out_ AMT_PALM_RUNTIME* Runtime
);

// Same best-effort registry persistence and clamp contract as the
// AmtPalmConfig* trio above, for AMT_POINTER_CONFIG.
_IRQL_requires_(PASSIVE_LEVEL)
VOID AmtPointerConfigLoadFromRegistry(_In_ WDFDEVICE Device, _Inout_ PAMT_POINTER_CONFIG Config);

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID AmtPointerConfigSaveToRegistry(_In_ WDFDEVICE Device, _In_ const AMT_POINTER_CONFIG* Config);

VOID AmtPointerConfigClamp(_Inout_ PAMT_POINTER_CONFIG Config);

_IRQL_requires_(PASSIVE_LEVEL)
VOID AmtScrollConfigLoadFromRegistry(_In_ WDFDEVICE Device, _Inout_ PAMT_SCROLL_CONFIG Config);

VOID AmtScrollConfigSaveToRegistry(_In_ WDFDEVICE Device, _In_ const AMT_SCROLL_CONFIG* Config);
VOID AmtScrollConfigClamp(_Inout_ PAMT_SCROLL_CONFIG Config);

VOID AmtPointerRuntimeRebuild(
    _In_ const AMT_POINTER_CONFIG* Config,
    _Out_ AMT_POINTER_RUNTIME* Runtime
);

VOID AmtScrollRuntimeRebuild(
    _In_ const AMT_SCROLL_CONFIG* Config,
    _Out_ AMT_SCROLL_RUNTIME* Runtime
);

EXTERN_C_END