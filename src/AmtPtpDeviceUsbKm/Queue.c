// Queue entry points and callbacks. Kernel-mode Driver Framework

#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmQueueInitialize)
#endif

NTSTATUS
AmtPtpDeviceUsbKmQueueInitialize(
    _In_ WDFDEVICE Device
    )
// Creates default parallel queue and manual queue for touch reads.
{
    WDFQUEUE queue;
    NTSTATUS status;
    WDF_IO_QUEUE_CONFIG    queueConfig;
    PDEVICE_CONTEXT        pDeviceContext;

    PAGED_CODE();

    pDeviceContext = DeviceGetContext(Device);
    
    // Default queue for non-forwarded requests.
    //
    // EvtIoDeviceControl (external/user-mode-originated IOCTLs) is
    // deliberately NOT wired up here: this FDO is a lower filter on the
    // HIDClass stack with no device interface of its own (see Public.h),
    // so no user-mode caller can ever reach it directly. The
    // AmtPtpConfigGui-facing IOCTL_AMT_PTP_* surface is served exclusively
    // by the separate KMDF control device - see
    // AmtPtpConfigControlEvtIoDeviceControl in ConfigIoctl.c.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

    queueConfig.EvtIoInternalDeviceControl = AmtPtpDeviceUsbKmEvtIoDeviceControl;
    queueConfig.EvtIoStop = AmtPtpDeviceUsbKmEvtIoStop;

    status = WdfIoQueueCreate(
                 Device,
                 &queueConfig,
                 WDF_NO_OBJECT_ATTRIBUTES,
                 &queue
                 );

    if( !NT_SUCCESS(status) ) {
        return status;
    }

    // Manual queue for forwarded HID read requests.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoStop = AmtPtpDeviceUsbKmEvtIoStop;

    status = WdfIoQueueCreate(
        Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &pDeviceContext->InputQueue
    );
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // NOTE: a separate manual "MouseInputQueue" for the force-touch mouse
    // top-level collection existed here between the "Fullfix" commit and
    // this revert. It has been removed - the mouse collection's reads are
    // forwarded to the same InputQueue as the digitizer, matching the
    // pre-Fullfix behavior (see AmtPtpDispatchReadReportRequests and
    // AmtPtpEvtUsbInterruptPipeReadComplete).

    return status;
}

VOID
AmtPtpDeviceUsbKmEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
// Dispatches HID IOCTLs to handler functions.
{
    NTSTATUS status;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT pDeviceContext = DeviceGetContext(device);
    BOOLEAN requestPending = FALSE;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    // DIAG: every IOCTL this filter sees except IOCTL_HID_READ_REPORT,
    // which is the per-frame hot path and would flood DebugView64 if
    // traced here. Everything else - descriptors, attributes, GET/
    // SET_FEATURE - only happens around enumeration and wake, so tracing
    // it (still gated by the usual DebugMode switch in AmtTrace()) is
    // cheap and gives the wake-time IOCTL timeline needed to correlate
    // against a System-log "MTConfig" failure - see the
    // WELLSPRING_MODE_SETFEATURE_* comment in Device.h.
    if (IoControlCode != IOCTL_HID_READ_REPORT) {
        AmtTrace(pDeviceContext, "EvtIoDeviceControl: ENTER, IoControlCode=0x%08X", IoControlCode);
    }

    switch (IoControlCode)
    {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = AmtPtpGetHidDescriptor(device, Request);
        break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = AmtPtpGetDeviceAttribs(device, Request);
        break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = AmtPtpGetReportDescriptor(device, Request);
        break;
    case IOCTL_HID_READ_REPORT:
        status = AmtPtpDispatchReadReportRequests(device, Request, &requestPending);
        break;
    case IOCTL_HID_GET_FEATURE:
        status = AmtPtpReportFeatures(device, Request);
        break;
    case IOCTL_HID_SET_FEATURE:
        status = AmtPtpSetFeatures(device, Request);
        break;
    case IOCTL_HID_GET_STRING:
    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
    case IOCTL_UMDF_HID_GET_INPUT_REPORT:
    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
    default:
        AmtTrace(pDeviceContext, "EvtIoDeviceControl: unsupported IoControlCode=0x%08X", IoControlCode);
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    if (IoControlCode != IOCTL_HID_READ_REPORT) {
        AmtTrace(pDeviceContext, "EvtIoDeviceControl: EXIT, IoControlCode=0x%08X, status=0x%08X",
            IoControlCode, status);
    }

    if (requestPending != TRUE) {
        WdfRequestComplete(Request, status);
    }

    return;
}

NTSTATUS
AmtPtpDispatchReadReportRequests(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _Out_ BOOLEAN* Pending
)
// Forwards HID read requests to the manual input queue.
{
    NTSTATUS status;
    PDEVICE_CONTEXT pDevContext;

    pDevContext = DeviceGetContext(Device);

    // REVERT (force-touch regression fix): the buffer-size-based split that
    // routed the mouse top-level collection's reads to a separate
    // MouseInputQueue was introduced in the "Fullfix" rework and is the
    // most likely cause of the intermittent (every-other-press, requiring
    // a full finger lift to recover) force-touch delivery failures - the
    // mouse collection's read cadence from mouhid.sys is not guaranteed to
    // keep a request continuously queued at the exact moment
    // AmtPtpEvtUsbInterruptPipeReadComplete needs one, so the down/up pulse
    // could desync from the digitizer-driven delivery loop.
    //
    // Restored to the pre-Fullfix design: every HID read request (both the
    // digitizer/touch collection and the force-touch mouse collection) is
    // forwarded to the single InputQueue. AmtPtpEvtUsbInterruptPipeReadComplete
    // opportunistically claims a second pending request off that same queue
    // for mouse delivery, exactly as before.
    status = WdfRequestForwardToIoQueue(
        Request,
        pDevContext->InputQueue
    );

    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    if (NULL != Pending) {
        *Pending = TRUE;
    }

exit:
    return status;
}

VOID
AmtPtpDeviceUsbKmEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
// Complete or acknowledge stop requests before power transition.
{
    UNREFERENCED_PARAMETER(Queue);

    // Suspend: acknowledge and let the framework requeue the request.
    if (ActionFlags & WdfRequestStopActionSuspend) {
        WdfRequestStopAcknowledge(Request, FALSE);
        return;
    }

    // Purge: cancel forwarded requests directly so stop/remove can finish.
    if (ActionFlags & WdfRequestStopActionPurge) {
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
}