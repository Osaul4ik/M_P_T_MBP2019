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
    // AUDIT FIX: this is where IOCTL_HID_READ_REPORT requests actually sit
    // pending (forwarded here via WdfRequestForwardToIoQueue, waiting on
    // AmtPtpEvtUsbInterruptPipeReadComplete) - EvtIoStop was previously only
    // wired up on the default queue above, so Suspend/Purge on THIS queue
    // (device stop/remove, "Safely Remove") had no handler at all and the
    // framework would wait indefinitely for these requests to be
    // acknowledged.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    queueConfig.EvtIoStop = AmtPtpDeviceUsbKmEvtIoStop;

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
// Called before device leaves D0 for power-managed queues.
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
    // actually be cancelled so the I/O manager can proceed.
    //
    // AUDIT FIX: WdfRequestCancelSentRequest is for a request the driver
    // itself sent onward via WdfRequestSend and is waiting on completion
    // for. This request was only forwarded to InputQueue via
    // WdfRequestForwardToIoQueue - it was never sent anywhere - so that call
    // does nothing to it. The request would then never be completed, and
    // WDF would wait forever for it during device stop/remove, hanging on
    // unplug/disable/"Safely Remove". Complete it directly instead.
    if (ActionFlags & WdfRequestStopActionPurge) {
        WdfRequestComplete(Request, STATUS_CANCELLED);
        return;
    }
}