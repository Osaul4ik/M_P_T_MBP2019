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

    // USB
    WDFUSBDEVICE    UsbDevice;
    WDFUSBPIPE      InterruptPipe;
    WDFUSBINTERFACE UsbInterface;
    WDFQUEUE        InputQueue;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    ULONG           UsbDeviceTraits;

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

    // Contact pool for PTPCore and ActiveContact.
    // ---------------------------------------------------------------
    ACTIVE_CONTACT ActiveContacts[MAX_CONTACTS];

    // Monotonic ContactID counter - never reuses an ID while "warm".
    // Every lift-off advances it; reseeded at D0Entry.
    ULONG   NextContactId;

    // QPC frequency cached at D0Entry
    LARGE_INTEGER PerfFrequency;

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