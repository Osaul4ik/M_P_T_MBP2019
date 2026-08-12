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
    queueConfig.EvtIoDeviceControl         = AmtPtpDeviceUsbKmEvtIoDeviceControlExternal;
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

VOID
AmtPtpDeviceUsbKmEvtIoDeviceControlExternal(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
    )
// Dispatches the AmtPtpConfigGui-facing IOCTL_AMT_PTP_* control codes
// (Public.h). Anything else falls through to STATUS_NOT_SUPPORTED - the
// HID surface is never reachable through this path, only through
// EvtIoInternalDeviceControl (HIDCLASS sits above this driver for that).
{
    NTSTATUS status;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    switch (IoControlCode)
    {
    case IOCTL_AMT_PTP_GET_PALM_CONFIG:
        status = AmtPtpGetPalmConfig(device, Request);
        break;
    case IOCTL_AMT_PTP_SET_PALM_CONFIG:
        status = AmtPtpSetPalmConfig(device, Request);
        break;
    case IOCTL_AMT_PTP_GET_PAD_GEOMETRY:
        status = AmtPtpGetPadGeometry(device, Request);
        break;
    case IOCTL_AMT_PTP_RESET_PALM_CONFIG:
        status = AmtPtpResetPalmConfig(device, Request);
        break;
    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestComplete(Request, status);
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