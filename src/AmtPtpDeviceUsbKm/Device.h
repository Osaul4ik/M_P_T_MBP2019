// Device.h - Device definitions. Kernel-mode Driver Framework

#include "public.h"
#include <Hid.h>
#include "ActiveContact.h"
#include "PTPCore.h"

EXTERN_C_START

// Force-touch vs hard-tap click arbitration (Ptpcore.c). While the
// mechanical button is held, the ordinary click report is withheld
// until PTPCore can tell whether the press is turning into a force
// touch (deep press) or staying an ordinary click - so a force touch
// never also fires a regular click underneath it. See ClickArbitration*
// fields below.
typedef enum _CLICK_ARBITRATION_STATE
{
    CLICK_ARBITRATION_IDLE = 0,    // button not down
    CLICK_ARBITRATION_PENDING,     // button down, still deciding
    CLICK_ARBITRATION_HARD_TAP,    // decided: ordinary click - report it
    CLICK_ARBITRATION_FORCE_TOUCH  // decided: force touch - suppress click
} CLICK_ARBITRATION_STATE;

typedef struct _DEVICE_CONTEXT
{
    // AUDIT FIX: the continuous USB reader can have more than one read
    // pending at once, so AmtPtpEvtUsbInterruptPipeReadComplete
    // (Interrupt.c) is not guaranteed to run to completion on one CPU
    // before the next completion starts on another. Every field below
    // that function reads/writes without doing its own synchronization
    // (LastReportTime, ScanTimeAccumulator, ActiveContacts[],
    // ClickArbitrationState, ForceTouch*, PendingForceTouchEdge*, and
    // everything PTPCore_ProcessFrame touches through them) must only be
    // touched while holding this lock. Created in
    // AmtPtpDeviceUsbKmCreateDevice; acquired/released around the whole
    // state-mutating body of AmtPtpEvtUsbInterruptPipeReadComplete.
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

    // PTP state
    BOOLEAN PtpInputOn;
    BOOLEAN PtpReportTouch;
    BOOLEAN PtpReportButton;

    // Previous frame's physical integrated-button state. Compared against
    // the current frame in PTPCore.c to detect the 0->1 click edge that
    // drives the forced-rebirth anti-jitter-snap workaround.
    BOOLEAN PrevButtonClicked;

    // Force-touch latch (Ptpcore.c). TRUE while some contact's pressure
    // is above FORCE_TOUCH_PRESSURE_THRESHOLD and the button is held;
    // compared against the previous frame to derive the down/up edges
    // Interrupt.c uses to pulse the synthetic right-click mouse report.
    BOOLEAN ForceTouchActive;

    // Force-touch drag lockout (Ptpcore.c). Anchor position latched at
    // the button-down edge; if a contact wanders past
    // FORCE_TOUCH_DRAG_LOCKOUT_DISTANCE from the anchor while the button
    // stays held, ForceTouchDragLockout latches TRUE for the rest of the
    // press. Feeds the click arbitration decision below - moving before
    // the force-touch pressure threshold was ever crossed commits the
    // press to an ordinary hard-tap click (e.g. dragging a window).
    // Once the press has already been arbitrated as a force touch, this
    // flag is ignored - dragging while holding a force touch is a
    // right-click-drag and must not cancel it. Reset on button release.
    BOOLEAN ForceTouchAnchorValid;
    USHORT  ForceTouchAnchorX;
    USHORT  ForceTouchAnchorY;
    BOOLEAN ForceTouchDragLockout;

    // AUDIT FIX: force-touch synthetic right-click delivery (Interrupt.c)
    // opportunistically claims a SECOND pending IOCTL_HID_READ_REPORT
    // request off InputQueue the moment a down/up edge fires. If no
    // second request happened to be queued that exact pass, the edge
    // used to be silently dropped - a real, reproducible way to lose a
    // force-touch click depending on mouhid.sys's read cadence.
    //
    // AUDIT FIX #2: a single "last edge wins" latch (the previous design)
    // only preserves the edge across ONE missed pass - if a second edge
    // (the matching up, for a fast press-and-release) arrives before a
    // mouse request becomes available, it overwrites the first and the
    // pair collapses to "nothing happened" - Windows never sees Button2
    // move at all, silently swallowing the whole click. A real FIFO fixes
    // this: each edge is queued and delivered in order, one per available
    // request per interrupt completion, so a fast down+up still reaches
    // Windows as two reports (possibly a frame or two late) instead of
    // cancelling out.
    //
    // Edges only ever fire on a genuine state change (Interrupt.c), so
    // they strictly alternate down/up/down/up - capacity 4 (two full
    // press-release cycles backed up) is a comfortable margin for any
    // realistic mouhid.sys read cadence. On the vanishingly unlikely case
    // the queue is completely full, the OLDEST pending edge is dropped to
    // make room for the newest - a stale edge from several read-cycles
    // ago is less relevant to the user than the most recent one.
#define PENDING_FORCE_TOUCH_EDGE_CAPACITY 4
    BOOLEAN PendingForceTouchEdgeQueue[PENDING_FORCE_TOUCH_EDGE_CAPACITY]; // each entry: Button2 state (1=down, 0=up)
    UCHAR   PendingForceTouchEdgeHead;   // index of the oldest queued edge
    UCHAR   PendingForceTouchEdgeCount;  // number of queued, undelivered edges

    // Click arbitration (Ptpcore.c) - see CLICK_ARBITRATION_STATE above.
    // PeakPressure/StartQpc/StallFrames are only meaningful while State ==
    // PENDING.
    CLICK_ARBITRATION_STATE ClickArbitrationState;
    USHORT                  ClickArbitrationPeakPressure;
    LONGLONG                ClickArbitrationStartQpc;
    // Consecutive frames PeakPressure has NOT grown (press has plateaued,
    // not actively climbing toward FORCE_TOUCH_PRESSURE_THRESHOLD). Lets
    // an ordinary flat/light press resolve to HARD_TAP well before the
    // full CLICK_ARBITRATION_TIMEOUT_MS safety-net timeout - only a press
    // that keeps climbing needs the full window measured out.
    UCHAR                   ClickArbitrationStallFrames;

    // Scan time
    LARGE_INTEGER LastReportTime;

    // SCANTIME FIX: Windows PTP spec requires Report.ScanTime to be a
    // free-running, ever-increasing hardware-clock counter (100us units),
    // which Windows itself differentiates frame-to-frame to derive
    // velocity/inertia - NOT a pre-computed inter-frame delta. This
    // accumulator holds the running total (in 100us units); each frame
    // adds the elapsed-since-last-report delta onto it, and the low 16
    // bits are truncated into the USHORT report field, letting it wrap
    // naturally as the spec expects. Reseeded to 0 at D0Entry alongside
    // LastReportTime.
    ULONG ScanTimeAccumulator;

    // Palm rejection - session-level latch (sticky "still palm-adjacent"
    // state), owned by PTPCore_ProcessFrame. Per-sample classification
    // lives in Palm.c.
    BOOLEAN PalmDetected;

    // Contact pool (PTPCore / ActiveContact). Pool POSITION is NOT
    // identity - ContactID is. See ActiveContact.h for the full
    // rationale (this replaces the old slot-indexed TRACK[] array).
    // ---------------------------------------------------------------
    ACTIVE_CONTACT ActiveContacts[MAX_CONTACTS];

    // Monotonic ContactID counter - never reuses an ID while "warm".
    // Every lift-off advances it; reseeded at D0Entry.
    ULONG   NextContactId;

    // QPC frequency cached at D0Entry
    LARGE_INTEGER PerfFrequency;

    // Overflow report queue - when PTPCore_ProcessFrame produces more
    // contact events (lift-offs, or DOWN/MOVE reports that lost the race
    // for a report slot) than remaining PTP_CORE_FRAME capacity, deferred
    // entries are drained at the front of the next frame. See
    // AmtCoreEmitContact/AmtCoreDrainOverflow in PTPCore.c.
    // ---------------------------------------------------------------
    ULONG         OverflowContactID[PTP_MAX_CONTACT_POINTS];
    USHORT        OverflowX[PTP_MAX_CONTACT_POINTS];
    USHORT        OverflowY[PTP_MAX_CONTACT_POINTS];
    CONTACT_PHASE OverflowPhase[PTP_MAX_CONTACT_POINTS];
    BOOLEAN       OverflowConfident[PTP_MAX_CONTACT_POINTS];
    UCHAR         OverflowCount;

    // Recent-lift memory for retap smoothing (PTPCore.h /
    // RECENT_LIFT_RING). Deliberately NOT slot-indexed - see PTPCore.h
    // for why the old SlotLastLiftQpc/X/Y[PTP_MAX_CONTACT_POINTS]
    // arrays were a slot-as-identity mistake.
    // ---------------------------------------------------------------
    RECENT_LIFT_RING RecentLifts;

} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

// AUDIT: WdfUsbTargetDeviceSendControlTransferSynchronously in
// AmtPtpSetWellspringMode previously ran with no send-options/timeout, so a
// stalled/malicious USB device or hub could block the calling thread
// (including the D0Entry power-up path) forever. 5s is generous for a
// single control transfer but keeps a hard upper bound.
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