// Device.h - Device definitions. Kernel-mode Driver Framework

#include "public.h"
#include <Hid.h>
#include "ActiveContact.h"
#include "Gesture.h"
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
    // press - a hard-tap-then-drag (e.g. moving a window) must never
    // trip a synthetic right-click mid-drag. Reset on button release.
    BOOLEAN ForceTouchAnchorValid;
    USHORT  ForceTouchAnchorX;
    USHORT  ForceTouchAnchorY;
    BOOLEAN ForceTouchDragLockout;

    // Click arbitration (Ptpcore.c) - see CLICK_ARBITRATION_STATE above.
    // PrevPressure/StartQpc are only meaningful while State == PENDING.
    CLICK_ARBITRATION_STATE ClickArbitrationState;
    USHORT                  ClickArbitrationPrevPressure;
    LONGLONG                ClickArbitrationStartQpc;

    // Scan time
    LARGE_INTEGER LastReportTime;

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

    // GestureEngine session state (Gesture.h). ACTIVE_CONTACT.WasInGesture
    // is SET FROM this by PTPCore.c, never the reverse.
    GESTURE_SESSION GestureSession;

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
