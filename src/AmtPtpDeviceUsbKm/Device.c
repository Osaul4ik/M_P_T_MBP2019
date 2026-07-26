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

    status         = STATUS_SUCCESS;
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

    // Reseed ContactID counter and reset the contact pool on D0Entry.
    // Prevents stale ContactIDs from surviving sleep/wake cycles.
    // NextContactId=0 reserved; first birth pre-increments to 1.
    pDeviceContext->NextContactId        = 0;
    AmtGestureSessionInit(&pDeviceContext->GestureSession);
    pDeviceContext->OverflowCount        = 0;
    pDeviceContext->PrevButtonClicked    = FALSE;
    pDeviceContext->ForceTouchActive     = FALSE;
    pDeviceContext->ForceTouchAnchorValid = FALSE;
    pDeviceContext->ForceTouchDragLockout = FALSE;
    pDeviceContext->ClickArbitrationState = CLICK_ARBITRATION_IDLE;
    AmtContactPoolInit(pDeviceContext->ActiveContacts);

    // Zero RecentLifts on D0Entry to prevent stale retap-smoothing
    // hints from a previous power session.
    RtlZeroMemory(&pDeviceContext->RecentLifts, sizeof(pDeviceContext->RecentLifts));

    status = AmtPtpSetWellspringMode(pDeviceContext, TRUE);
    if (!NT_SUCCESS(status)) {
        status = STATUS_SUCCESS;
    }

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

// AmtPtpSetHapticFeedback
//
// Ported from the SET_REPORT mechanism confirmed identical in both
// drivers/hid/hid-magicmouse.c (Linux) and vitoplantamura/
// MagicTrackpad2ForWindows (Windows) for the Magic Trackpad 2. See the
// long comment next to HAPTIC_INTERFACE_INDEX in AppleDefinition.h -
// UNVERIFIED on this MacBookPro16,1 internal T2 trackpad. Best-effort:
// a failure here should not prevent the device from otherwise working,
// same as the existing AmtPtpSetWellspringMode call sites already treat
// its own failures as non-fatal.

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetHapticFeedback(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ ULONG           FeedbackClick,
    _In_ ULONG           FeedbackRelease)
{
    NTSTATUS                     status;
    WDF_USB_CONTROL_SETUP_PACKET setupPacket;
    WDF_MEMORY_DESCRIPTOR        memoryDescriptor;
    WDF_REQUEST_SEND_OPTIONS     sendOptions;
    ULONG                        cbTransferred;

    // Payload template from mt2_click/mt2_release in both reference
    // drivers (byte 0, the report ID, is passed separately as the low
    // byte of wValue below - not part of the transfer buffer, matching
    // the Windows fork's usb_control_msg call which strips it too).
    UCHAR clickBuffer[]   = { 0x01, 0x00, 0x78, 0x02, 0x00, 0x24, 0x30, 0x06, 0x01, 0x00, 0x18, 0x48, 0x13 };
    UCHAR releaseBuffer[] = { 0x01, 0x00, 0x78, 0x02, 0x00, 0x24, 0x30, 0x06, 0x01, 0x00, 0x18, 0x48, 0x13 };

    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
        &sendOptions, WDF_REL_TIMEOUT_IN_SEC(WELLSPRING_CONTROL_TRANSFER_TIMEOUT_SEC));

    // Offsets 1, 4, 9 here == offsets 2, 5, 10 in the reference sources'
    // 14-byte buffer (report ID included there at index 0) - same bytes,
    // shifted down by one since our buffer excludes the report ID.
    clickBuffer[1] = (UCHAR)(FeedbackClick >> 0);
    clickBuffer[4] = (UCHAR)(FeedbackClick >> 8);
    clickBuffer[9] = (UCHAR)(FeedbackClick >> 16);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memoryDescriptor, clickBuffer, sizeof(clickBuffer));
    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestHostToDevice, BmRequestToInterface,
        HAPTIC_FEEDBACK_REQUEST_ID,
        (USHORT)(HAPTIC_REPORT_TYPE_FEATURE | HAPTIC_REPORTID_CLICK),
        HAPTIC_INTERFACE_INDEX);
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DeviceContext->UsbDevice, WDF_NO_HANDLE, &sendOptions,
        &setupPacket, &memoryDescriptor, &cbTransferred);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    releaseBuffer[1] = (UCHAR)(FeedbackRelease >> 0);
    releaseBuffer[4] = (UCHAR)(FeedbackRelease >> 8);
    releaseBuffer[9] = (UCHAR)(FeedbackRelease >> 16);

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memoryDescriptor, releaseBuffer, sizeof(releaseBuffer));
    WDF_USB_CONTROL_SETUP_PACKET_INIT(
        &setupPacket,
        BmRequestHostToDevice, BmRequestToInterface,
        HAPTIC_FEEDBACK_REQUEST_ID,
        (USHORT)(HAPTIC_REPORT_TYPE_FEATURE | HAPTIC_REPORTID_RELEASE),
        HAPTIC_INTERFACE_INDEX);
    setupPacket.Packet.bm.Request.Type = BmRequestClass;

    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        DeviceContext->UsbDevice, WDF_NO_HANDLE, &sendOptions,
        &setupPacket, &memoryDescriptor, &cbTransferred);

    return status;
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

    // Haptic actuator setup - deliberately BEFORE the TYPE3 early-return
    // below: TYPE3 (T2) devices skip the normal Wellspring mode-switch
    // read/modify/write entirely, but the actuator profile is a
    // completely separate SET_REPORT and still needs configuring on
    // T2 hardware. Best-effort: ignore failure here, same treatment
    // AmtPtpEvtDeviceD0Entry already gives AmtPtpSetWellspringMode's own
    // status. See AppleDefinition.h for why this is unverified on T2.
    if (IsWellspringModeOn) {
        NTSTATUS hapticStatus = AmtPtpSetHapticFeedback(
            DeviceContext, HAPTIC_FEEDBACK_CLICK_DEFAULT, HAPTIC_FEEDBACK_RELEASE_DEFAULT);
        UNREFERENCED_PARAMETER(hapticStatus);
    }

    if (DeviceContext->DeviceInfo->tp_type == TYPE3) {
        DeviceContext->IsWellspringModeOn = IsWellspringModeOn;
        return STATUS_SUCCESS;
    }

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
