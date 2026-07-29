// Queue entry points and callbacks. Kernel-mode Driver Framework

#include "driver.h"
#include "queue.tmh"

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

    // Manual queue for touch read requests.
    //
    // AUDIT FIX (EvtIoStop wired to the wrong queue): EvtIoStop was
    // previously only ever set on the DEFAULT queue's config above, but the
    // requests that actually need Purge/Suspend handling are the
    // IOCTL_HID_READ_REPORT requests sitting HERE, in InputQueue -
    // AmtPtpDispatchReadReportRequests (below) forwards them out of the
    // default queue with WdfRequestForwardToIoQueue the moment they arrive,
    // so by the time the default queue's EvtIoStop could ever see one, it
    // has already left that queue. The default queue's own requests
    // (GET_DESCRIPTOR/GET_ATTRIBUTES/GET_REPORT_DESCRIPTOR/GET_FEATURE/
    // SET_FEATURE, all handled synchronously in
    // AmtPtpDeviceUsbKmEvtIoDeviceControl) never stay pending long enough
    // for EvtIoStop to matter there. Registering the same callback here
    // too means a request parked in InputQueue waiting on
    // AmtPtpEvtUsbInterruptPipeReadComplete is now actually acknowledged
    // (Suspend) or completed with STATUS_CANCELLED (Purge) instead of
    // relying on undocumented default framework purge behavior for a
    // manual, non-power-managed queue.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoStop    = AmtPtpDeviceUsbKmEvtIoStop;

    status = WdfIoQueueCreate(
        Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &pDeviceContext->InputQueue
    );

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
    BOOLEAN requestPending = FALSE;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

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
        status = STATUS_NOT_SUPPORTED;
        break;
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
// Shared EvtIoStop for BOTH queues (see AmtPtpDeviceUsbKmQueueInitialize):
// the default power-managed queue (Suspend on D0 exit, Purge on removal)
// and the manual, non-power-managed InputQueue (Purge on removal/stop only
// - a manual queue isn't power-managed, so Suspend is not expected for it,
// but ActionFlags is still checked rather than assumed).
//
// AUDIT FIX: this previously only traced and returned, which violates the
// WDF contract - EvtIoStop MUST complete, cancel, or explicitly acknowledge
// the request, or the framework will wait indefinitely for it and block the
// power transition (sleep/hibernate/device-stop) that triggered the call.
{
    UNREFERENCED_PARAMETER(Queue);

    // Suspend: request is merely paused - acknowledge in place and the
    // framework will hand it back to us (or the driver requeues it) once
    // the device returns to D0. FALSE = do not requeue ourselves; nothing
    // else on this driver owns manual requeue logic for this queue.
    if (ActionFlags & WdfRequestStopActionSuspend) {
        WdfRequestStopAcknowledge(Request, FALSE);
        return;
    }

    // Purge: device is being removed/stopped for good - the request must
    // actually be completed so the I/O manager can proceed.
    //
    // BUG FIX: this used to call WdfRequestCancelSentRequest, which is for
    // a request the driver itself sent onward to a lower I/O target via
    // WdfRequestSend. This request was never sent anywhere - it sits in
    // InputQueue after being forwarded here with WdfRequestForwardToIoQueue
    // (see AmtPtpDispatchReadReportRequests) and is waiting to be picked up
    // by AmtPtpEvtUsbInterruptPipeReadComplete. WdfRequestCancelSentRequest
    // does not complete a request like this, so the request was left
    // neither completed nor cancelled - exactly the framework hang the
    // AUDIT FIX comment above (EvtIoStop's completion contract) exists to
    // avoid. Completing it directly is correct here.
    if (ActionFlags & WdfRequestStopActionPurge) {
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
}
