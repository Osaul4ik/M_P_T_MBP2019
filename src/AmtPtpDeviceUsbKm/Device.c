// Device.c - Device handling events. Kernel-mode Driver Framework

#include "driver.h"
#include "Match.h" // MATCH_MAX_TIME_DELTA_100NS, for the D0Entry tick-cache

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmCreateDevice)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDevicePrepareHardware)
#pragma alloc_text (PAGE, AmtPtpEvtDeviceReleaseHardware)
// Both PASSIVE_LEVEL-only and both already call PAGED_CODE(); previously
// missing here, which left them PAGED_CODE()-asserting from a non-paged
// segment (PREfast C28172).
#pragma alloc_text (PAGE, AmtPtpEvtDeviceD0Exit)
#pragma alloc_text (PAGE, SelectInterruptInterface)
#pragma alloc_text (PAGE, AmtPtpCreateConfigControlDevice)
#endif

#define ACTIVE_CONTACTS_ALIGNMENT 64
#define ACTIVE_CONTACTS_POOL_TAG  'ApAc' // "AmtPtp Active Contacts"

// Allocates the ActiveContacts pool on a real 64-byte boundary. 
static PACTIVE_CONTACT
AmtAllocateAlignedContactPool(VOID)
{
    SIZE_T    rawSize;
    PVOID     raw;
    ULONG_PTR aligned;

    rawSize = (MAX_CONTACTS * sizeof(ACTIVE_CONTACT)) + ACTIVE_CONTACTS_ALIGNMENT + sizeof(PVOID);

    raw = ExAllocatePoolZero(NonPagedPoolNx, rawSize, ACTIVE_CONTACTS_POOL_TAG);
    if (raw == NULL) {
        return NULL;
    }

    aligned = ((ULONG_PTR)raw + sizeof(PVOID) + (ACTIVE_CONTACTS_ALIGNMENT - 1))
              & ~(ULONG_PTR)(ACTIVE_CONTACTS_ALIGNMENT - 1);

    NT_ASSERT((aligned & (ACTIVE_CONTACTS_ALIGNMENT - 1)) == 0);
    *((PVOID*)(aligned - sizeof(PVOID))) = raw;

    return (PACTIVE_CONTACT)aligned;
}

static VOID
AmtFreeAlignedContactPool(_In_opt_ PACTIVE_CONTACT Pool)
{
    PVOID raw;

    if (Pool == NULL) {
        return;
    }

    raw = *((PVOID*)((ULONG_PTR)Pool - sizeof(PVOID)));
    ExFreePoolWithTag(raw, ACTIVE_CONTACTS_POOL_TAG);
}

// Read the matching config entry for the detected device.
//
// DESIGN NOTE: an idProduct with no table entry is a deliberate best-effort
// fallback to Bcm5974ConfigTable[0] (a TYPE4/T2 profile), not an error -
// this lets the driver still bind (with possibly miscalibrated geometry
// limits) on unlisted-but-compatible hardware instead of refusing to load.
// If that's no longer the desired behavior, change this to return NULL and
// have AmtPtpDeviceUsbKmEvtDevicePrepareHardware fail the bind instead.
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

    return NULL;
}

// AmtPtpCreateConfigControlDevice
//
// Creates a real KMDF control device for the configuration GUI. The PnP
// device remains a lower filter; the GUI endpoint is deliberately separate
// from the USB/HID stack so CreateFile() is handled by KMDF rather than being
// forwarded to the lower USB driver.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpCreateConfigControlDevice(_In_ WDFDEVICE TargetDevice)
{
    PWDFDEVICE_INIT       controlInit = NULL;
    WDF_OBJECT_ATTRIBUTES controlAttributes;
    WDF_IO_QUEUE_CONFIG    queueConfig;
    WDFDEVICE              controlDevice = NULL;
    PAMT_CONFIG_CONTROL_CONTEXT controlContext;
    NTSTATUS                status;

    PAGED_CODE();

    DECLARE_CONST_UNICODE_STRING(sddl,
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)");
    DECLARE_CONST_UNICODE_STRING(ntName,
        L"\\Device\\AmtPtpDeviceUsbKm");
    DECLARE_CONST_UNICODE_STRING(dosName,
        L"\\DosDevices\\AmtPtpDeviceUsbKm");

    controlInit = WdfControlDeviceInitAllocate(
        WdfDeviceGetDriver(TargetDevice),
        &sddl);

    if (controlInit == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = WdfDeviceInitAssignName(controlInit, &ntName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(controlInit);
        return status;
    }

    WdfDeviceInitSetExclusive(controlInit, FALSE);

    // Wire up EvtFileClose so a handle closing for ANY reason - including
    // the GUI process dying without running its own cleanup - is caught by
    // the driver itself and used to force LiveEnabled back off.
    {
        WDF_FILEOBJECT_CONFIG fileConfig;
        WDF_OBJECT_ATTRIBUTES fileContextAttributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
            &fileContextAttributes,
            AMT_CONFIG_CONTROL_FILE_CONTEXT);

        WDF_FILEOBJECT_CONFIG_INIT(
            &fileConfig,
            WDF_NO_EVENT_CALLBACK,             // EvtDeviceFileCreate
            AmtPtpConfigControlEvtFileClose,   // EvtFileClose
            WDF_NO_EVENT_CALLBACK);            // EvtFileCleanup

        WdfDeviceInitSetFileObjectConfig(
            controlInit,
            &fileConfig,
            &fileContextAttributes);
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
        &controlAttributes,
        AMT_CONFIG_CONTROL_CONTEXT);
    controlAttributes.EvtCleanupCallback = AmtPtpConfigControlEvtConfigControlCleanup;
    controlAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfDeviceCreate(
        &controlInit,
        &controlAttributes,
        &controlDevice);

    if (!NT_SUCCESS(status)) {
        // controlInit came from WdfControlDeviceInitAllocate (not from
        // EvtDriverDeviceAdd), so on a failed WdfDeviceCreate the driver -
        // not the framework - owns freeing it. Missing this leaked the
        // WDFDEVICE_INIT structure on this error path.
        WdfDeviceInitFree(controlInit);
        return status;
    }

    controlContext = AmtConfigControlGetContext(controlDevice);
    controlContext->TargetDevice = TargetDevice;

    status = WdfDeviceCreateSymbolicLink(
        controlDevice,
        &dosName);

    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchSequential);

    queueConfig.EvtIoDeviceControl = AmtPtpConfigControlEvtIoDeviceControl;

    status = WdfIoQueueCreate(
        controlDevice,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(controlDevice);
        return status;
    }

    // A control device does not receive I/O until this call has completed.
    WdfControlFinishInitializing(controlDevice);

    DeviceGetContext(TargetDevice)->ConfigControlDevice = controlDevice;
    return STATUS_SUCCESS;
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
    pnpPowerCallbacks.EvtDeviceReleaseHardware = AmtPtpEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry         = AmtPtpEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit          = AmtPtpEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = AmtPtpEvtDeviceContextCleanup;

    // NOTE: this FDO is a lower filter on the HIDClass stack and exposes no
    // device interface of its own (GUID_DEVINTERFACE_AmtPtpDeviceUsbKm was
    // removed - see Public.h). User-mode (AmtPtpConfigGui) talks to the
    // separate KMDF control device instead (AmtPtpCreateConfigControlDevice
    // below), via its own DOS symbolic link and SDDL. No name/SDDL is
    // needed on this device object itself.
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Reverted alongside the WdfDeviceInitSetPowerPolicyOwnership(TRUE)
    // claim in AmtPtpDeviceUsbKmEvtDeviceAdd (Driver.c): this call is only
    // valid when this driver is the power-policy owner (see the
    // FDOPowerPolicyOwnerAPI rule), and that ownership claim was reverted
    // as a likely cause of a STATUS_DEVICE_DATA_ERROR seen after it was
    // introduced. The AmtPtpDeviceUsbKm_AddReg SelectiveSuspendEnabled=1
    // INF setting is left in place; whichever driver actually owns power
    // policy in this stack negotiates USB selective suspend on its own -
    // this driver does not need to (and, without ownership, cannot)
    // configure S0-idle settings itself.

    deviceContext = DeviceGetContext(device);
    RtlZeroMemory(deviceContext, sizeof(DEVICE_CONTEXT));

    // Timer used to restart the interrupt pipe's continuous reader with
    // backoff after AmtPtpEvtUsbInterruptReadersFailed (see Interrupt.c).
    // Its callback calls WdfIoTargetStart, which requires PASSIVE_LEVEL,
    // hence the explicit passive execution level here rather than the
    // default DISPATCH_LEVEL timer callback.
    {
        WDF_TIMER_CONFIG      timerConfig;
        WDF_OBJECT_ATTRIBUTES timerAttributes;

        WDF_TIMER_CONFIG_INIT(&timerConfig, AmtPtpEvtReaderRestartTimer);

        WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
        timerAttributes.ParentObject   = device;
        timerAttributes.ExecutionLevel = WdfExecutionLevelPassive;

        status = WdfTimerCreate(
            &timerConfig, &timerAttributes, &deviceContext->ReaderRestartTimer);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    deviceContext->ActiveContacts = AmtAllocateAlignedContactPool();
    if (deviceContext->ActiveContacts == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    deviceContext->PtpReportButton = TRUE;
    deviceContext->PtpReportTouch  = TRUE;

    // Palm-rejection config: compiled-in defaults first (so the device is
    // always in a valid state even if registry access below fails), then
    // best-effort overridden from the registry if a GUI previously saved
    // custom values there.
    {
        AMT_PALM_CONFIG defaultCfg = AMT_PALM_CONFIG_DEFAULT_INIT;
        deviceContext->PalmConfig = defaultCfg;
    }
    AmtPalmConfigLoadFromRegistry(device, &deviceContext->PalmConfig);
    AmtPalmRuntimeRebuild(&deviceContext->PalmConfig, &deviceContext->PalmRuntime);

    // Pointer config (Force Tap threshold + action): same compiled-in-
    // defaults-then-registry-override sequence as PalmConfig above.
    {
        AMT_POINTER_CONFIG defaultPointerCfg = AMT_POINTER_CONFIG_DEFAULT_INIT;
        deviceContext->PointerConfig = defaultPointerCfg;
    }
    AmtPointerConfigLoadFromRegistry(device, &deviceContext->PointerConfig);
    AmtPointerRuntimeRebuild(&deviceContext->PointerConfig, &deviceContext->PointerRuntime);

    {
        AMT_SCROLL_CONFIG defaultScrollCfg = AMT_SCROLL_CONFIG_DEFAULT_INIT;
        deviceContext->ScrollConfig = defaultScrollCfg;
    }
    AmtScrollConfigLoadFromRegistry(device, &deviceContext->ScrollConfig);
    AmtScrollRuntimeRebuild(&deviceContext->ScrollConfig, &deviceContext->ScrollRuntime);

    // Create the shared state lock for frame processing.
    {
        WDF_OBJECT_ATTRIBUTES lockAttributes;
        WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
        lockAttributes.ParentObject = device;

        status = WdfSpinLockCreate(&lockAttributes, &deviceContext->StateLock);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        status = WdfSpinLockCreate(&lockAttributes, &deviceContext->LiveLock);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    // The GUI talks to a separate KMDF control device.
    // This physical device remains a lower filter; no user-mode device
    // interface is created on the filter FDO itself.
    status = AmtPtpCreateConfigControlDevice(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AmtPtpDeviceUsbKmQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
    }

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
        return STATUS_NOT_SUPPORTED;
    }

    // Fail closed if device metadata contains an impossible packet offset or
    // vendor-mode buffer index. These are table values, but runtime checks
    // keep future/edited entries from turning parser mistakes into OOB access.
    if (pDeviceContext->DeviceInfo->tp_fsize == 0 ||
        pDeviceContext->DeviceInfo->um_size == 0 ||
        pDeviceContext->DeviceInfo->um_size > 8 ||
        pDeviceContext->DeviceInfo->um_switch_idx >= pDeviceContext->DeviceInfo->um_size) {
        return STATUS_INVALID_PARAMETER;
    }

    // Only TYPE4/TYPE5 packets carry a real pressure field (see
    // AppleDefinition.h - pressure was introduced with TYPE4). Everything
    // older (TYPE1-3, e.g. WELLSPRING8) reports zeros there, so treat those
    // trackpads as force-touch-incapable rather than let stale/zero
    // pressure feed the arbitration state machine.
    pDeviceContext->SupportsForceTouch =
        (pDeviceContext->DeviceInfo->tp_type == TYPE4) ||
        (pDeviceContext->DeviceInfo->tp_type == TYPE5);

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

// AmtPtpEvtDeviceReleaseHardware
//
// Symmetric counterpart to AmtPtpDeviceUsbKmEvtDevicePrepareHardware. Per
// the KMDF PnP contract, EvtDevicePrepareHardware can be invoked more than
// once over the lifetime of a single device object (e.g. resource
// rebalance), and without a matching EvtDeviceReleaseHardware the second
// PrepareHardware call would see stale UsbInterface/InterruptPipe/UsbDevice
// handles from the previous hardware instantiation instead of cleanly
// re-acquiring them.

NTSTATUS
AmtPtpEvtDeviceReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourceListTranslated)
{
    PDEVICE_CONTEXT pDeviceContext;

    UNREFERENCED_PARAMETER(ResourceListTranslated);
    PAGED_CODE();

    pDeviceContext = DeviceGetContext(Device);

    // WdfUsbTargetDeviceCreate's returned WDFUSBDEVICE, and the interface/
    // pipe handles derived from it via SelectInterruptInterface, are all
    // parented to Device and torn down by the framework automatically -
    // this callback only needs to drop this driver's own references to
    // them so AmtPtpDeviceUsbKmEvtDevicePrepareHardware's
    // "if (pDeviceContext->UsbDevice == NULL)" guard re-acquires a fresh
    // set on the next PrepareHardware instead of skipping acquisition and
    // reusing now-invalid handles.
    pDeviceContext->InterruptPipe = NULL;
    pDeviceContext->UsbInterface  = NULL;
    pDeviceContext->UsbDevice     = NULL;

    return STATUS_SUCCESS;
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

    pDeviceContext  = DeviceGetContext(Device);
    isTargetStarted = FALSE;

    // A fresh D0Entry always means a clean slate for the reader-recovery
    // escalation ladder in Interrupt.c - whatever failure streak happened
    // in a previous power session is irrelevant now.
    pDeviceContext->ReaderRecoveryStage = READER_RECOVERY_RESET_PIPE;

    pDeviceContext->LastReportTime =
        KeQueryPerformanceCounter(&pDeviceContext->PerfFrequency);

    // MICRO-OPT: precompute PerfFrequency-derived tick thresholds once here
    // instead of on every call site (see Device.h field comments). 0 when
    // there's no usable clock, matching each site's original PerfFrequencyHz
    // <= 0 guard exactly.
    if (pDeviceContext->PerfFrequency.QuadPart > 0) {
        pDeviceContext->RetapWindowTicks =
            (RETAP_WINDOW_100NS * pDeviceContext->PerfFrequency.QuadPart) / 10000000LL;
        pDeviceContext->MatchMaxTimeDeltaTicks =
            (MATCH_MAX_TIME_DELTA_100NS * pDeviceContext->PerfFrequency.QuadPart) / 10000000LL;
        pDeviceContext->ClickArbitrationTimeoutTicks =
            (pDeviceContext->PerfFrequency.QuadPart * CLICK_ARBITRATION_TIMEOUT_MS) / 1000;
        pDeviceContext->ScanTimeScaleQ16 =
            (10000LL << 16) / pDeviceContext->PerfFrequency.QuadPart;
    } else {
        pDeviceContext->RetapWindowTicks             = 0;
        pDeviceContext->MatchMaxTimeDeltaTicks        = 0;
        pDeviceContext->ClickArbitrationTimeoutTicks = 0;

        // OPTIMIZATION: fold the "no usable clock" fallback into the same
        // Q16 scale used by the normal path, instead of branching on
        // PerfFrequency on every single interrupt completion (see
        // AmtPtpEvtUsbInterruptPipeReadComplete). The old fallback did
        // "PerfDelta /= 100LL"; solving (x*scale)>>16 == x/100 for scale
        // gives 65536/100 = 655.36, rounded to 655 - same approximation,
        // computed once here instead of re-branched every completion. This
        // path is already the degenerate "no usable clock" case, so the
        // extra ~0.05% rounding difference from 655 vs 655.36 is moot.
        pDeviceContext->ScanTimeScaleQ16             = 655;
    }

    // Reset per-session timing and contact state on D0 entry.
    pDeviceContext->ScanTimeAccumulator = 0;
    pDeviceContext->NextContactId        = 0;
    pDeviceContext->OverflowCount        = 0;
    pDeviceContext->PrevButtonClicked    = FALSE;
    pDeviceContext->ForceTouchAnchorValid = FALSE;
    pDeviceContext->ForceTouchDragLockout = FALSE;
    // Drop pending/in-flight force-touch clicks from the previous power
    // session - nothing was delivered to a live HID client across a D0
    // transition, so there's no in-flight UP to worry about orphaning.
    pDeviceContext->ForceTouchDeliveryState     = FORCE_TOUCH_DELIVERY_IDLE;
    pDeviceContext->PendingForceTouchClickCount = 0;
    pDeviceContext->ClickArbitrationState = CLICK_ARBITRATION_IDLE;
    AmtContactPoolInit(pDeviceContext->ActiveContacts);

    // Zero RecentLifts on D0Entry to prevent stale retap-smoothing
    // hints from a previous power session.
    RtlZeroMemory(&pDeviceContext->RecentLifts, sizeof(pDeviceContext->RecentLifts));

    // Wellspring-mode setup is best-effort; only the start result matters.
    //
    // After a real power-off (PreviousState == WdfPowerDeviceD3Final), the
    // device may not yet accept control transfers in the very first
    // instant it reappears in D0 - re-enumeration/link-training on the
    // parent hub can still be settling. A single fire-and-forget attempt
    // here silently left Wellspring mode never re-armed if that first
    // attempt lost the race, which reads to the user as "trackpad
    // unresponsive after wake" even though the interrupt pipe itself comes
    // up fine. Retry a bounded few times with a short pause; a D0Entry
    // reached from a lighter (non-D3Final) transition doesn't need this
    // slack, so it gets a single attempt as before.
    {
        ULONG maxAttempts = (PreviousState == WdfPowerDeviceD3Final)
            ? WELLSPRING_MODE_D0ENTRY_MAX_ATTEMPTS
            : 1;
        ULONG attempt;

        for (attempt = 0; attempt < maxAttempts; attempt++) {
            if (NT_SUCCESS(AmtPtpSetWellspringMode(pDeviceContext, TRUE))) {
                break;
            }

            if (attempt + 1 < maxAttempts) {
                LARGE_INTEGER delay;
                // D0Entry runs at PASSIVE_LEVEL, so a short blocking pause
                // here is sanctioned by the WDF power-callback contract -
                // this is not a hot path.
                delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(
                    WELLSPRING_MODE_D0ENTRY_RETRY_DELAY_MS_UNIT * (attempt + 1));
                KeDelayExecutionThread(KernelMode, FALSE, &delay);
            }
        }
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

    // Cancel any pending backed-off reader-restart synchronously (wait for
    // an in-progress callback to finish) before tearing down the pipe's
    // I/O target below. Without this, a timer that fires concurrently with
    // D0Exit could call WdfIoTargetStart on a target that is being (or has
    // just been) stopped for this power transition.
    WdfTimerStop(pDeviceContext->ReaderRestartTimer, TRUE);

    WdfIoTargetStop(
        WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
        WdfIoTargetCancelSentIo);

    AmtPtpSetWellspringMode(pDeviceContext, FALSE);

    return STATUS_SUCCESS;
}

// AmtPtpEvtDeviceContextCleanup
// Frees the manually-aligned ActiveContacts pool allocated in AmtPtpDeviceUsbKmCreateDevice. 

VOID
AmtPtpEvtDeviceContextCleanup(
    _In_ WDFOBJECT Device)
{
    PDEVICE_CONTEXT pDeviceContext;
    pDeviceContext = DeviceGetContext((WDFDEVICE)Device);

    // The control device is owned by this PnP device instance. Delete it
    // before releasing the physical device context so the GUI endpoint
    // cannot outlive the target WDFDEVICE.
    if (pDeviceContext->ConfigControlDevice != NULL) {
        PAMT_CONFIG_CONTROL_CONTEXT controlContext =
            AmtConfigControlGetContext(pDeviceContext->ConfigControlDevice);
        if (controlContext != NULL)
            controlContext->TargetDevice = NULL;

        WdfObjectDelete(pDeviceContext->ConfigControlDevice);
        pDeviceContext->ConfigControlDevice = NULL;
    }

    AmtFreeAlignedContactPool(pDeviceContext->ActiveContacts);
    pDeviceContext->ActiveContacts = NULL;
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

    // TYPE3 devices (WELLSPRING8 - MacBookAir6,x/7,x, 2013-2017) stream
    // multitouch data without a vendor mode switch - matches Linux
    // bcm5974_wellspring_mode(): "Type 3 does not require a mode switch".
    // Every T2 entry (TYPE4) still falls through to the read/modify/write
    // sequence below as before.
    if (DeviceContext->DeviceInfo->tp_type == TYPE3) {
        DeviceContext->IsWellspringModeOn = IsWellspringModeOn;
        return STATUS_SUCCESS;
    }

    if (DeviceContext->DeviceInfo->um_size == 0 ||
        DeviceContext->DeviceInfo->um_size > sizeof(buffer) ||
        DeviceContext->DeviceInfo->um_switch_idx >= DeviceContext->DeviceInfo->um_size) {
        return STATUS_INVALID_PARAMETER;
    }

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