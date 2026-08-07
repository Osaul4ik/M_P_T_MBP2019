// Device.h - Device definitions. Kernel-mode Driver Framework

#include "public.h"
#include <Hid.h>
#include "ActiveContact.h"
#include "PTPCore.h"

EXTERN_C_START

// Hold ordinary click reports until the press is classified.
typedef enum _CLICK_ARBITRATION_STATE
{
    CLICK_ARBITRATION_IDLE = 0,    // button not down
    CLICK_ARBITRATION_PENDING,     // button down, still deciding
    CLICK_ARBITRATION_HARD_TAP,    // decided: ordinary click - report it
    CLICK_ARBITRATION_FORCE_TOUCH  // decided: force touch - suppress click
} CLICK_ARBITRATION_STATE;

typedef struct _DEVICE_CONTEXT
{
    // Protect shared frame-processing state across concurrent USB completions.
    WDFSPINLOCK     StateLock;

    // USB - touched every interrupt/report cycle.
    WDFUSBDEVICE    UsbDevice;
    WDFUSBPIPE      InterruptPipe;
    WDFQUEUE        InputQueue;

    // Device config
    const struct BCM5974_CONFIG* DeviceInfo;
    BOOLEAN IsWellspringModeOn;

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

    // Queue synthetic right-click edges until they can be delivered.
#define PENDING_FORCE_TOUCH_EDGE_CAPACITY 4
    BOOLEAN PendingForceTouchEdgeQueue[PENDING_FORCE_TOUCH_EDGE_CAPACITY]; // each entry: Button2 state (1=down, 0=up)
    UCHAR   PendingForceTouchEdgeHead;   // index of the oldest queued edge
    UCHAR   PendingForceTouchEdgeCount;  // number of queued, undelivered edges

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

EXTERN_C_END