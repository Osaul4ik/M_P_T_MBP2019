// Device.c - Device handling events. Kernel-mode Driver Framework

#include "driver.h"
#include "device.tmh"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmCreateDevice)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDevicePrepareHardware)
#endif

// AmtPtpGetDeviceConfig
//
// AUDIT FIX (#7): takes the descriptor by pointer instead of by value -
// avoids an unnecessary full-struct copy onto the stack on every call.

_IRQL_requires_(PASSIVE_LEVEL)
static const struct BCM5974_CONFIG*
AmtPtpGetDeviceConfig(_In_ const PUSB_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    USHORT id = DeviceDescriptor->idProduct;
    const struct BCM5974_CONFIG* cfg;

    for (cfg = Bcm5974ConfigTable; cfg->identification; ++cfg) {
        if (cfg->identification == id)
            return cfg;
    }

    return &Bcm5974ConfigTable[0];
}

// AmtPtpDeviceUsbKmCreateDevice

NTSTATUS
AmtPtpDeviceUsbKmCreateDevice(_Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES        deviceAttributes;
    PDEVICE_CONTEXT              deviceContext;
    WDFDEVICE                    device;
    NTSTATUS                     status;

    PAGED_CODE();

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = AmtPtpDeviceUsbKmEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry         = AmtPtpEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit          = AmtPtpEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status))
        return status;

    deviceContext = DeviceGetContext(device);
    RtlZeroMemory(deviceContext, sizeof(DEVICE_CONTEXT));

    deviceContext->PtpReportButton = TRUE;
    deviceContext->PtpReportTouch  = TRUE;

    // AUDIT FIX (data race): see the StateLock comment in Device.h.
    // Parented to the device so WDF tears it down automatically; no
    // explicit delete needed.
    {
        WDF_OBJECT_ATTRIBUTES lockAttributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
        lockAttributes.ParentObject = device;

        status = WdfSpinLockCreate(&lockAttributes, &deviceContext->StateLock);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    status = WdfDeviceCreateDeviceInterface(
        device, &GUID_DEVINTERFACE_AmtPtpDeviceUsbKm, NULL);

    if (NT_SUCCESS(status))
        status = AmtPtpDeviceUsbKmQueueInitialize(device);

    return status;
}

// AmtPtpDeviceUsbKmEvtDevicePrepareHardware

NTSTATUS
AmtPtpDeviceUsbKmEvtDevicePrepareHardware(
    _In_ WDFDEVICE     Device,
    _In_ WDFCMRESLIST  ResourceList,
    _In_ WDFCMRESLIST  ResourceListTranslated)
{
    NTSTATUS         status;
    PDEVICE_CONTEXT  pDeviceContext;
    WDF_USB_DEVICE_INFORMATION deviceInfo;

    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);
    PAGED_CODE();

    pDeviceContext = DeviceGetContext(Device);

    if (pDeviceContext->UsbDevice == NULL) {
        status = WdfUsbTargetDeviceCreate(
            Device, WDF_NO_OBJECT_ATTRIBUTES, &pDeviceContext->UsbDevice);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    WdfUsbTargetDeviceGetDeviceDescriptor(
        pDeviceContext->UsbDevice, &pDeviceContext->DeviceDescriptor);

    pDeviceContext->DeviceInfo = AmtPtpGetDeviceConfig(&pDeviceContext->DeviceDescriptor);
    if (pDeviceContext->DeviceInfo == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);
    status = WdfUsbTargetDeviceRetrieveInformation(pDeviceContext->UsbDevice, &deviceInfo);
    if (NT_SUCCESS(status)) {

        pDeviceContext->UsbDeviceTraits = deviceInfo.Traits;
    } else {
        pDeviceContext->UsbDeviceTraits = 0;
    }

    status = SelectInterruptInterface(Device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AmtPtpConfigContReaderForInterruptEndPoint(pDeviceContext);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    pDeviceContext->PtpReportButton = TRUE;
    pDeviceContext->PtpReportTouch  = TRUE;

    return status;
}

// AmtPtpEvtDeviceD0Entry

NTSTATUS
AmtPtpEvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT pDeviceContext;
    NTSTATUS        status;
    BOOLEAN         isTargetStarted;

    UNREFERENCED_PARAMETER(PreviousState);

    pDeviceContext  = DeviceGetContext(Device);
    isTargetStarted = FALSE;

    pDeviceContext->LastReportTime =
        KeQueryPerformanceCounter(&pDeviceContext->PerfFrequency);

    // AUDIT FIX (data race): same StateLock as
    // AmtPtpEvtUsbInterruptPipeReadComplete - the interrupt pipe isn't
    // started again until after this reset (WdfIoTargetStart below), but
    // WdfIoTargetStop on the way into D0Exit only guarantees outstanding
    // I/O is cancelled, not that a completion routine already running on
    // another CPU has finished touching these fields. Taking the lock here
    // closes that window.
    WdfSpinLockAcquire(pDeviceContext->StateLock);

    // Reseed ContactID counter and reset the contact pool on D0Entry.
    // Prevents stale ContactIDs from surviving sleep/wake cycles.
    // NextContactId=0 reserved; first birth pre-increments to 1.
    pDeviceContext->NextContactId        = 0;
    pDeviceContext->OverflowCount        = 0;
    pDeviceContext->PrevButtonClicked    = FALSE;
    pDeviceContext->ForceTouchActive     = FALSE;
    pDeviceContext->ForceTouchAnchorValid = FALSE;
    pDeviceContext->ForceTouchDragLockout = FALSE;
    // Any pending, undelivered force-touch edges from a previous power
    // session are meaningless once the button state has been reset above -
    // delivering them now would risk a bogus right-click sequence on the
    // very first frames after resume. Drop them, not carry them forward.
    pDeviceContext->PendingForceTouchEdgeHead  = 0;
    pDeviceContext->PendingForceTouchEdgeCount = 0;
    pDeviceContext->ClickArbitrationState = CLICK_ARBITRATION_IDLE;
    AmtContactPoolInit(pDeviceContext->ActiveContacts);

    // Zero RecentLifts on D0Entry to prevent stale retap-smoothing
    // hints from a previous power session.
    RtlZeroMemory(&pDeviceContext->RecentLifts, sizeof(pDeviceContext->RecentLifts));

    WdfSpinLockRelease(pDeviceContext->StateLock);

    // Wellspring-mode failure here is intentionally non-fatal - status is
    // deliberately NOT propagated, only the WdfIoTargetStart result below
    // is (this matches the pre-existing behavior; the standalone
    // "status = STATUS_SUCCESS" reset previously here was redundant, since
    // status is unconditionally overwritten by the next assignment either
    // way).
    (VOID)AmtPtpSetWellspringMode(pDeviceContext, TRUE);

    status = WdfIoTargetStart(
        WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe));
    if (!NT_SUCCESS(status)) {
        goto end;
    }
    isTargetStarted = TRUE;

end:
    if (!NT_SUCCESS(status) && isTargetStarted) {
        WdfIoTargetStop(
            WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
            WdfIoTargetCancelSentIo);
    }

    return status;
}

// AmtPtpEvtDeviceD0Exit

NTSTATUS
AmtPtpEvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT pDeviceContext;

    UNREFERENCED_PARAMETER(TargetState);
    PAGED_CODE();

    pDeviceContext = DeviceGetContext(Device);

    WdfIoTargetStop(
        WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
        WdfIoTargetCancelSentIo);

    AmtPtpSetWellspringMode(pDeviceContext, FALSE);

    return STATUS_SUCCESS;
}

// SelectInterruptInterface

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(_In_ WDFDEVICE Device)
{
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
    NTSTATUS                            status = STATUS_SUCCESS;
    PDEVICE_CONTEXT                     pDeviceContext;
    WDFUSBPIPE                          pipe;
    WDF_USB_PIPE_INFORMATION            pipeInfo;
    UCHAR                               index;
    UCHAR                               numberConfiguredPipes;

    PAGED_CODE();

    pDeviceContext = DeviceGetContext(Device);
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

    status = WdfUsbTargetDeviceSelectConfig(
        pDeviceContext->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &configParams);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    pDeviceContext->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
    numberConfiguredPipes        = configParams.Types.SingleInterface.NumberConfiguredPipes;

    for (index = 0; index < numberConfiguredPipes; index++) {
        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        pipe = WdfUsbInterfaceGetConfiguredPipe(
            pDeviceContext->UsbInterface, index, &pipeInfo);

        WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

        if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
            pDeviceContext->InterruptPipe = pipe;
            break;
        }
    }

    if (!pDeviceContext->InterruptPipe) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    return STATUS_SUCCESS;
}

// AmtPtpSetWellspringMode

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ BOOLEAN         IsWellspringModeOn)
{
    NTSTATUS                    status;
    WDF_USB_CONTROL_SETUP_PACKET setupPacket;
    WDF_MEMORY_DESCRIPTOR       memoryDescriptor;
    WDF_REQUEST_SEND_OPTIONS    sendOptions;
    ULONG                       cbTransferred;
    // OPTIMIZATION: um_size is at most 8 bytes across every USBMSG_TYPEn
    // table entry (statically guaranteed by the C_ASSERT block next to
    // those defines in AppleDefinition.h) - a WdfMemoryCreate/WdfObjectDelete
    // pool round-trip for this was unnecessary overhead and an extra
    // failure path for what's really just a few bytes on the stack.
    UCHAR                       buffer[8] = { 0 };

    // AUDIT FIX: bound both control transfers below so a stalled device/hub
    // can't hang the calling thread (this runs on the D0Entry path) forever.
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
        &sendOptions, WDF_REL_TIMEOUT_IN_SEC(WELLSPRING_CONTROL_TRANSFER_TIMEOUT_SEC));

    // NOTE: every entry in Bcm5974ConfigTable (fallback + all T2 variants,
    // including the confirmed 16" 0x0340) is built via DATAFORMAT(TYPE4) -
    // none use TYPE3. A prior TYPE3 early-return here ("T2 devices skip
    // the mode-switch") was therefore dead code: it never matched any
    // configured device, so every real device already executes the
    // read/modify/write sequence below. Removed rather than left in to
    // avoid implying a code path exists that doesn't.

    NT_ASSERT(DeviceContext->DeviceInfo->um_size <= sizeof(buffer));

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &memoryDescriptor, buffer, (ULONG)DeviceContext->DeviceInfo->um_size);

    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestDeviceToHost, BmRequestToInterface,
        BCM5974_WELLSPRING_MODE_READ_REQUEST_ID,
        (USHORT)DeviceContext->DeviceInfo->um_req_val,
        (USHORT)DeviceContext->DeviceInfo->um_req_idx);
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DeviceContext->UsbDevice, WDF_NO_HANDLE, &sendOptions,
        &setupPacket, &memoryDescriptor, &cbTransferred);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    buffer[DeviceContext->DeviceInfo->um_switch_idx] = IsWellspringModeOn
        ? (unsigned char)DeviceContext->DeviceInfo->um_switch_on
        : (unsigned char)DeviceContext->DeviceInfo->um_switch_off;

    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestHostToDevice, BmRequestToInterface,
        BCM5974_WELLSPRING_MODE_WRITE_REQUEST_ID,
        (USHORT)DeviceContext->DeviceInfo->um_req_val,
        (USHORT)DeviceContext->DeviceInfo->um_req_idx);
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DeviceContext->UsbDevice, WDF_NO_HANDLE, &sendOptions,
        &setupPacket, &memoryDescriptor, &cbTransferred);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    DeviceContext->IsWellspringModeOn = IsWellspringModeOn;

    return status;
}
