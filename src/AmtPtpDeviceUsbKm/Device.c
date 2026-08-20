// Device.c - Device handling events. Kernel-mode Driver Framework

#include "driver.h"
#include "Match.h" // MATCH_MAX_TIME_DELTA_100NS, for the D0Entry tick-cache

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmCreateDevice)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDevicePrepareHardware)
// AmtPtpEvtDeviceReleaseHardware and AmtPtpEvtDeviceD0Exit are
// deliberately NOT listed here (and no longer call PAGED_CODE() either -
// see the comment at each definition). Both are PASSIVE_LEVEL-only, but
// both also call WdfSpinLockAcquire (D0ExitLock) as part of the two-level
// synchronization model in Device.h, which briefly raises IRQL to
// DISPATCH_LEVEL. Code actually placed in the PAGE section must never run
// above APC_LEVEL - if it faults in at raised IRQL the system bugchecks -
// so alloc_text(PAGE, ...) on a spinlock-acquiring function is a genuine
// correctness bug, not just a PREfast nag (PREfast still catches it as
// C28150, "IRQL above the maximum acceptable for the function", which is
// how this was originally found). A prior revision added alloc_text(PAGE,
// ...) here specifically to silence PAGED_CODE()-without-alloc_text
// (C28172); that traded one real diagnostic for a worse one. The
// underlying fix is to keep this code resident instead: these callbacks
// already never run on a hot path, so the extra nonpaged footprint is
// immaterial next to the correctness requirement.
#pragma alloc_text (PAGE, SelectInterruptInterface)
#pragma alloc_text (PAGE, AmtPtpAcquireConfigControlDevice)
// Detaches from the driver-lifetime control device via WDFWAITLOCK
// (AmtPtpAcquireConfigControlDevice's counterpart) - PASSIVE_LEVEL only,
// same as that function.
#pragma alloc_text (PAGE, AmtPtpEvtDeviceContextCleanup)
// Same WDFWAITLOCK-only, PASSIVE_LEVEL-only shape as
// AmtPtpAcquireConfigControlDevice above - safe to page, called from every
// ConfigIoctl.c handler (PASSIVE_LEVEL default queue) instead of each of
// them reading TargetDevice unlocked.
#pragma alloc_text (PAGE, AmtPtpConfigControlSnapshotTargetDevice)
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
// CORRECTION: an earlier comment here claimed an idProduct with no table
// entry fell back to Bcm5974ConfigTable[0] - that was never true of the
// code below, which simply returns NULL on no match. NULL is the correct,
// fail-closed behavior: AmtPtpDeviceUsbKmEvtDevicePrepareHardware already
// treats a NULL DeviceInfo as STATUS_NOT_SUPPORTED and refuses to bind. In
// practice this path is unreachable anyway - the INF's [Standard.NT$ARCH$]
// section lists only the exact VID/PID/MI combinations PnP will ever offer
// this driver for - but the function must still fail closed rather than
// guess if a future INF entry and this table ever drift apart.
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

// AmtPtpAcquireConfigControlDevice
//
// Gives the calling FDO ownership of the GUI-facing KMDF control device -
// creating it on the very first call from any FDO instance, and simply
// re-pointing it at TargetDevice on every later call.
//
// BACKGROUND / WHY NOT ONE CONTROL DEVICE PER FDO (as before):
//
// This device's parent USB hub does a real surprise-removal/re-enumeration
// on most sleep/wake cycles (see the D0Entry/D0Exit trace around
// WdfPowerDeviceD3 -> STATUS_NO_SUCH_DEVICE -> D3Final -> ReleaseHardware
// in the sleep/wake investigation) - a brand new PDO/FDO pair gets created
// afterward, not a resume of the same one. The control device previously
// went through the same per-FDO lifecycle: created in EvtDeviceAdd under
// the fixed name L"\\Device\\AmtPtpDeviceUsbKm", deleted in the old FDO's
// AmtPtpEvtDeviceContextCleanup.
//
// A KMDF control device with an open user-mode handle is NOT force-closed
// by surprise removal the way the FDO's own handles are - WdfObjectDelete
// on it simply waits for the last handle to close. AmtPtpConfigGui keeps
// exactly one such handle open for its entire lifetime (DeviceIo.cs -
// TryConnect() is called once; there is no SystemEvents.PowerModeChanged
// or WM_POWERBROADCAST handling to close and reopen it around sleep). So
// with the GUI running across a sleep/wake cycle:
//
//   1. The old FDO is surprise-removed; its EvtDeviceContextCleanup runs,
//      but WdfObjectDelete on the old control device cannot complete
//      while the GUI's handle is still open - the name stays reserved.
//   2. The parent hub re-enumerates; a new FDO's EvtDeviceAdd runs and
//      tries to create a NEW control device under the SAME fixed name.
//      WdfDeviceInitAssignName fails with STATUS_OBJECT_NAME_COLLISION
//      because the old one has not actually been deleted yet.
//   3. That failure propagates out of EvtDeviceAdd, so the new FDO never
//      comes up at all - not "slow", completely dead - until whatever
//      unblocks the old handle (closing the GUI) lets the old control
//      device finish being deleted, freeing the name for the FDO created
//      on the NEXT sleep/wake cycle.
//
// Making the control device driver-lifetime instead of FDO-lifetime
// removes the collision structurally: its name is claimed exactly once
// per driver load, so no later EvtDeviceAdd ever contests it. The GUI's
// handle can then legitimately stay open across any number of surprise
// removals - AMT_CONFIG_CONTROL_CONTEXT::TargetDevice just goes NULL
// between the old FDO's cleanup and the new FDO's AddDevice, causing
// ConfigIoctl.c's handlers to fail cleanly (STATUS_DEVICE_NOT_READY-style
// "no target device") instead of the whole stack refusing to load.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpAcquireConfigControlDevice(_In_ WDFDEVICE TargetDevice)
{
    PDRIVER_CONTEXT             driverContext;
    PAMT_CONFIG_CONTROL_CONTEXT controlContext;
    NTSTATUS                    status;

    PAGED_CODE();

    driverContext = DriverGetContext(WdfDeviceGetDriver(TargetDevice));

    // Guards both branches below: creation (first caller only) and
    // re-attachment (every later caller, including a fast surprise-
    // removal/re-enumeration racing AmtPtpEvtDeviceContextCleanup's
    // detach for the FDO instance being replaced). Never held across an
    // I/O completion or anything that could stall - just the pointer
    // read/create/write below.
    WdfWaitLockAcquire(driverContext->ConfigControlDeviceLock, NULL);

    if (driverContext->ConfigControlDevice != NULL) {
        // Not the first FDO instance this driver load has seen - the
        // control device already exists, just move it onto the new FDO.
        controlContext = AmtConfigControlGetContext(driverContext->ConfigControlDevice);
        controlContext->TargetDevice = TargetDevice;

        WdfWaitLockRelease(driverContext->ConfigControlDeviceLock);
        return STATUS_SUCCESS;
    }

    // First FDO instance this driver load has seen - create the control
    // device now. Everything below is unchanged from the original
    // per-FDO implementation except where noted.
    {
        PWDFDEVICE_INIT       controlInit = NULL;
        WDF_OBJECT_ATTRIBUTES controlAttributes;
        WDF_IO_QUEUE_CONFIG   queueConfig;
        WDFDEVICE             controlDevice = NULL;

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
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto unlock_and_return;
        }

        status = WdfDeviceInitAssignName(controlInit, &ntName);
        if (!NT_SUCCESS(status)) {
            WdfDeviceInitFree(controlInit);
            goto unlock_and_return;
        }

        WdfDeviceInitSetExclusive(controlInit, FALSE);

        // Wire up EvtFileClose so a handle closing for ANY reason -
        // including the GUI process dying without running its own
        // cleanup - is caught by the driver itself and used to force
        // LiveEnabled back off.
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
            // EvtDriverDeviceAdd), so on a failed WdfDeviceCreate the
            // driver - not the framework - owns freeing it.
            WdfDeviceInitFree(controlInit);
            goto unlock_and_return;
        }

        controlContext = AmtConfigControlGetContext(controlDevice);
        controlContext->TargetDevice = TargetDevice;

        status = WdfDeviceCreateSymbolicLink(
            controlDevice,
            &dosName);

        if (!NT_SUCCESS(status)) {
            WdfObjectDelete(controlDevice);
            goto unlock_and_return;
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
            goto unlock_and_return;
        }

        // A control device does not receive I/O until this call completes.
        WdfControlFinishInitializing(controlDevice);

        // Published only now that the control device is fully live -
        // AmtPtpEvtDeviceContextCleanup and the next AddDevice both read
        // this under the same lock, so neither can observe a half-built
        // control device.
        driverContext->ConfigControlDevice = controlDevice;
        status = STATUS_SUCCESS;
    }

unlock_and_return:
    WdfWaitLockRelease(driverContext->ConfigControlDeviceLock);
    return status;
}

// AmtPtpConfigControlSnapshotTargetDevice / AmtPtpConfigControlReleaseTargetDevice
//
// See the prototype comment in Device.h for the full use-after-free
// rationale. Locked read of AMT_CONFIG_CONTROL_CONTEXT::TargetDevice under
// the same ConfigControlDeviceLock every writer already uses, PLUS a
// WdfObjectReference taken on the handle before the lock is released - so
// the reference-take is atomic with the read and can never land on a
// handle that AmtPtpEvtDeviceContextCleanup has already started tearing
// down. The reference keeps the FDO's WDF object (and therefore its
// DEVICE_CONTEXT) allocated until the matching
// AmtPtpConfigControlReleaseTargetDevice call, regardless of how far PnP
// remove processing for that FDO has otherwise progressed in the meantime.
_IRQL_requires_(PASSIVE_LEVEL)
WDFDEVICE
AmtPtpConfigControlSnapshotTargetDevice(_In_ WDFDEVICE ControlDevice)
{
    PDRIVER_CONTEXT             driverContext;
    PAMT_CONFIG_CONTROL_CONTEXT controlContext;
    WDFDEVICE                   targetDevice;

    PAGED_CODE();

    driverContext  = DriverGetContext(WdfDeviceGetDriver(ControlDevice));
    controlContext = AmtConfigControlGetContext(ControlDevice);

    WdfWaitLockAcquire(driverContext->ConfigControlDeviceLock, NULL);
    targetDevice = controlContext->TargetDevice;
    if (targetDevice != NULL) {
        WdfObjectReference(targetDevice);
    }
    WdfWaitLockRelease(driverContext->ConfigControlDeviceLock);

    return targetDevice;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtPtpConfigControlReleaseTargetDevice(_In_opt_ WDFDEVICE TargetDevice)
{
    if (TargetDevice != NULL) {
        WdfObjectDereference(TargetDevice);
    }
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
    // separate, driver-lifetime KMDF control device instead
    // (AmtPtpAcquireConfigControlDevice below), via its own DOS symbolic
    // link and SDDL. No name/SDDL is needed on this device object itself.
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

        // Explicit, not relying on the (TRUE) default: this callback's own
        // synchronization is entirely D0ExitLock/RecoveryLock/
        // RecoveryGeneration (see Device.h) - it does not rely on, and must
        // not rely on, WDF's automatic-serialization device callback lock,
        // which is a separate mechanism scoped to the device's default
        // PASSIVE/DISPATCH callback synchronization and not coordinated
        // with the lifecycle locks this driver defines itself.
        timerConfig.AutomaticSerialization = FALSE;

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

    // Debug-trace switch: same compiled-in-default(FALSE)-then-registry-
    // override sequence as the config blocks above/below. See Trace.h for
    // what DebugMode does and how it differs from LiveEnabled.
    AmtTraceLoadDebugModeFromRegistry(device, deviceContext);

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

        // MS-RECOMMENDED SYNCHRONIZATION: guards D0ExitInProgress/
        // ReaderRecoveryStage/RecoveryGeneration together with a consistent
        // snapshot of InterruptPipe/UsbDevice - see the two-level
        // synchronization model comment on D0ExitLock/RecoveryLock in
        // Device.h for why a WDFSPINLOCK, not WDFWAITLOCK (PASSIVE_LEVEL-
        // only, disqualified by the DISPATCH_LEVEL
        // AmtPtpEvtUsbInterruptReadersFailed caller) or a bare Interlocked
        // flag (individually atomic ops, not atomic as the check-then-read
        // sequence this needs), is the correct primitive here.
        status = WdfSpinLockCreate(&lockAttributes, &deviceContext->D0ExitLock);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        // PASSIVE_LEVEL-only counterpart to D0ExitLock - serializes the
        // actual (blocking) recovery/D0Exit/ReleaseHardware cleanup work so
        // WdfIoTargetStop/WdfUsbTargetPipeResetSynchronously/
        // WdfUsbTargetDeviceResetPortSynchronously/AmtPtpCyclePort/
        // WdfIoTargetStart never run concurrently against each other. See
        // the Device.h comment on RecoveryLock for the full model.
        status = WdfWaitLockCreate(&lockAttributes, &deviceContext->RecoveryLock);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    // The GUI talks to a separate KMDF control device that is
    // driver-lifetime, not FDO-lifetime (see AmtPtpAcquireConfigControlDevice
    // for why a per-FDO control device cannot survive surprise removal/
    // re-enumeration). This call creates it on the first FDO instance and
    // just re-points it at `device` on every later one; the corresponding
    // detach happens in AmtPtpEvtDeviceContextCleanup below, and the
    // corresponding delete happens once, at driver unload, in
    // AmtPtpDeviceUsbKmEvtDriverContextCleanup (Driver.c). This physical
    // device remains a lower filter; no user-mode device interface is
    // created on the filter FDO itself.
    status = AmtPtpAcquireConfigControlDevice(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = AmtPtpDeviceUsbKmQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        AmtTrace(deviceContext, "CreateDevice: QueueInitialize FAILED, status=0x%08X", status);
    }

    return status;
}

// ---------------------------------------------------------------------
// PrepareHardware low-resources retry
// ---------------------------------------------------------------------
// WdfUsbTargetDeviceCreate, WdfUsbTargetDeviceSelectConfig (inside
// SelectInterruptInterface below) and WdfUsbTargetPipeConfigContinuousReader
// (inside AmtPtpConfigContReaderForInterruptEndPoint, Interrupt.c) all
// allocate pool internally on this driver's behalf. Every one of them was
// already NT_SUCCESS-checked and fails closed - nothing dereferences on
// failure. `verifier /query` on this module confirms that fail-closed
// behavior is exactly what's being exercised: "Randomized low resources
// simulation" (0x00000004) deliberately fails exactly one pool allocation
// for this driver per boot ("Pool Allocations Failed Deliberately: 1") to
// prove drivers handle it instead of bugchecking or leaking - which this
// one already did, with no crash and no bytes leaked.
//
// The remaining problem is PnP-level, not memory-safety. Unlike
// AmtPtpEvtDeviceD0Entry below - which WDF re-invokes on every sleep/wake,
// and which already retries SetWellspringMode/WdfIoTargetStart through
// AmtPtpD0EntryRetry - EvtDevicePrepareHardware runs exactly once per PnP
// Start, including the automatic ReleaseHardware -> PrepareHardware
// re-arrival cycle visible in the DebugView trace after some sleep/wake
// transitions (see the DIAG comments on both of those callbacks). If the
// verifier's one deliberate failure - or any other transient
// STATUS_INSUFFICIENT_RESOURCES, e.g. genuine low memory - lands inside
// that re-arrival's PrepareHardware, there is no second Start attempt: PnP
// fails the device with Code 10 and nothing, neither this driver nor the
// OS, retries a failed Start automatically. The device is then stuck until
// the user manually disables/re-enables it or it is physically
// re-enumerated.
//
// A short bounded retry restricted to STATUS_INSUFFICIENT_RESOURCES closes
// that gap the same way AmtPtpD0EntryRetry already does for the
// sleep/wake path, without masking a genuine, persistent
// resource-exhaustion condition - that case still exhausts the retry
// budget and fails exactly as before.
//
// If the retry budget above is exhausted anyway (a genuinely persistent
// condition, or simply bad luck with the retry timing), the three
// allocation-sensitive failure paths below also call WdfDeviceSetFailed(
// Device, WdfDeviceFailedAttemptRestart). Per its documented contract,
// this asks the PnP manager to reenumerate the device and give
// EvtDevicePrepareHardware a fresh Start attempt automatically - the
// documented alternative to a driver silently returning failure and
// leaving the device parked at Code 10 until the user intervenes or a
// physical re-enumeration happens to occur on its own. WDF caps
// consecutive restart attempts internally, so a truly persistent failure
// still gives up and fails normally rather than looping forever. This is
// deliberately NOT applied to the STATUS_NOT_SUPPORTED /
// STATUS_INVALID_PARAMETER paths further down (unrecognized hardware, bad
// DeviceInfo table entry) - those are structural, not transient, and no
// number of restarts will ever change their outcome.
#define PREPARE_HARDWARE_ALLOC_MAX_ATTEMPTS   3
#define PREPARE_HARDWARE_ALLOC_RETRY_DELAY_MS 20

typedef NTSTATUS
(*PFN_AMT_PREPARE_HW_ATTEMPT)(_In_ WDFDEVICE Device);

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
AmtPtpPrepareHardwareRetryOnLowResources(
    _In_ WDFDEVICE                  Device,
    _In_ PDEVICE_CONTEXT            DeviceContext,
    _In_ PCSTR                      OperationName,
    _In_ PFN_AMT_PREPARE_HW_ATTEMPT Attempt
    )
{
    ULONG    attempt;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    for (attempt = 0; attempt < PREPARE_HARDWARE_ALLOC_MAX_ATTEMPTS; attempt++) {
        status = Attempt(Device);
        AmtTrace(DeviceContext, "PrepareHardware: %s attempt %lu/%lu -> status=0x%08X",
            OperationName, attempt + 1, PREPARE_HARDWARE_ALLOC_MAX_ATTEMPTS, status);

        if (NT_SUCCESS(status) || status != STATUS_INSUFFICIENT_RESOURCES) {
            // Success, or a non-resource failure: stop immediately either
            // way. Only STATUS_INSUFFICIENT_RESOURCES is treated as the
            // transient condition worth retrying - anything else (bad
            // device state, unsupported hardware, etc.) is a real failure
            // and must take the normal error path unchanged.
            break;
        }

        if (attempt + 1 < PREPARE_HARDWARE_ALLOC_MAX_ATTEMPTS) {
            LARGE_INTEGER delay;
            // EvtDevicePrepareHardware is documented PASSIVE_LEVEL (and
            // PAGED_CODE()-asserted in the caller), so a short blocking
            // pause here is the same sanctioned trade-off
            // AmtPtpD0EntryRetry already makes on the sleep/wake path.
            delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(
                PREPARE_HARDWARE_ALLOC_RETRY_DELAY_MS * (attempt + 1));
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
    }

    return status;
}

// Adapter shims giving WdfUsbTargetDeviceCreate, SelectInterruptInterface
// and AmtPtpConfigContReaderForInterruptEndPoint the single-WDFDEVICE-
// argument shape AmtPtpPrepareHardwareRetryOnLowResources' function
// pointer expects, so all three share the one retry loop above instead of
// three near-identical copies of it.

static NTSTATUS
AmtPtpPrepareHwCreateUsbDeviceAttempt(_In_ WDFDEVICE Device)
{
    PDEVICE_CONTEXT pDeviceContext = DeviceGetContext(Device);

    return WdfUsbTargetDeviceCreate(
        Device, WDF_NO_OBJECT_ATTRIBUTES, &pDeviceContext->UsbDevice);
}

static NTSTATUS
AmtPtpPrepareHwSelectInterruptInterfaceAttempt(_In_ WDFDEVICE Device)
{
    return SelectInterruptInterface(Device);
}

static NTSTATUS
AmtPtpPrepareHwConfigContReaderAttempt(_In_ WDFDEVICE Device)
{
    return AmtPtpConfigContReaderForInterruptEndPoint(DeviceGetContext(Device));
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
    // DeviceGetContext cannot actually return NULL for a valid WDFDEVICE
    // (the context space is allocated at WdfDeviceCreate time via
    // WDF_OBJECT_ATTRIBUTES on this same object), but PREfast has no way
    // to know that from the macro alone and, once AmtTrace()'s own NULL
    // check on its Ctx parameter is inlined below, propagates that
    // possibility onto every later dereference of this pointer - hence
    // the explicit assumption here instead of scattering NULL checks
    // through otherwise-unconditional lifecycle code.
    _Analysis_assume_(pDeviceContext != NULL);

    // DIAG: lifecycle tracing (Code 10 / STATUS_DEVICE_DATA_ERROR
    // investigation after sleep-wake). Enabled only when this device's
    // "DebugMode" registry value is non-zero (see Trace.h) - view live
    // with Sysinternals DebugView64 (run as admin, Capture Kernel +
    // Enable Verbose Kernel Output both checked) or a kernel debugger.
    // Grep/filter on "[AmtPtp]" - every line shares that prefix.
    AmtTrace(pDeviceContext, "PrepareHardware: ENTER, UsbDevice=%p",
        pDeviceContext->UsbDevice);

    if (pDeviceContext->UsbDevice == NULL) {
        status = AmtPtpPrepareHardwareRetryOnLowResources(
            Device, pDeviceContext, "WdfUsbTargetDeviceCreate",
            AmtPtpPrepareHwCreateUsbDeviceAttempt);
        if (!NT_SUCCESS(status)) {
            AmtTrace(pDeviceContext, "PrepareHardware: WdfUsbTargetDeviceCreate FAILED, status=0x%08X",
                status);
            // See the WdfDeviceSetFailed rationale comment above
            // AmtPtpPrepareHardwareRetryOnLowResources: ask PnP for a real
            // Start retry, but only for the transient resource-exhaustion
            // case the retry loop above was for - a non-resource failure
            // here (e.g. a genuinely broken USB stack handle) would just
            // waste PnP's limited consecutive-restart budget on an outcome
            // that cannot change.
            if (status == STATUS_INSUFFICIENT_RESOURCES) {
                WdfDeviceSetFailed(Device, WdfDeviceFailedAttemptRestart);
            }
            return status;
        }
    }

    // As above with pDeviceContext itself: by this point UsbDevice is
    // non-NULL either because it already was (the if above was skipped)
    // or because AmtPtpPrepareHardwareRetryOnLowResources returned success,
    // which - via the AmtPtpPrepareHwCreateUsbDeviceAttempt function
    // pointer - means WdfUsbTargetDeviceCreate populated it. PREfast
    // cannot follow that through the indirect call, so it still treats
    // UsbDevice as possibly NULL on every use below; assume it away here
    // rather than adding a redundant runtime check.
    _Analysis_assume_(pDeviceContext->UsbDevice != NULL);

    WdfUsbTargetDeviceGetDeviceDescriptor(
        pDeviceContext->UsbDevice, &pDeviceContext->DeviceDescriptor);

    pDeviceContext->DeviceInfo = AmtPtpGetDeviceConfig(&pDeviceContext->DeviceDescriptor);
    if (pDeviceContext->DeviceInfo == NULL) {
        AmtTrace(pDeviceContext, "PrepareHardware: unrecognized idProduct=0x%04X, STATUS_NOT_SUPPORTED",
            pDeviceContext->DeviceDescriptor.idProduct);
        return STATUS_NOT_SUPPORTED;
    }

    // Fail closed if device metadata contains an impossible packet offset or
    // vendor-mode buffer index. These are table values, but runtime checks
    // keep future/edited entries from turning parser mistakes into OOB access.
    if (pDeviceContext->DeviceInfo->tp_fsize == 0 ||
        pDeviceContext->DeviceInfo->um_size == 0 ||
        pDeviceContext->DeviceInfo->um_size > 8 ||
        pDeviceContext->DeviceInfo->um_switch_idx >= pDeviceContext->DeviceInfo->um_size) {
        AmtTrace(pDeviceContext, "PrepareHardware: bad DeviceInfo table entry, STATUS_INVALID_PARAMETER");
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

    status = AmtPtpPrepareHardwareRetryOnLowResources(
        Device, pDeviceContext, "SelectInterruptInterface",
        AmtPtpPrepareHwSelectInterruptInterfaceAttempt);
    if (!NT_SUCCESS(status)) {
        AmtTrace(pDeviceContext, "PrepareHardware: SelectInterruptInterface FAILED, status=0x%08X",
            status);
        // SelectInterruptInterface can also fail with STATUS_INVALID_DEVICE_STATE
        // (no interrupt pipe among the configured ones) - a structural
        // mismatch, not the transient low-resources condition the retry
        // loop above targets, so only request a restart for that specific
        // status, exactly as done for WdfUsbTargetDeviceCreate above.
        if (status == STATUS_INSUFFICIENT_RESOURCES) {
            WdfDeviceSetFailed(Device, WdfDeviceFailedAttemptRestart);
        }
        return status;
    }

    status = AmtPtpPrepareHardwareRetryOnLowResources(
        Device, pDeviceContext, "ConfigContReaderForInterruptEndPoint",
        AmtPtpPrepareHwConfigContReaderAttempt);
    if (!NT_SUCCESS(status)) {
        AmtTrace(pDeviceContext, "PrepareHardware: ConfigContReaderForInterruptEndPoint FAILED, status=0x%08X",
            status);
        // Same reasoning: AmtPtpConfigContReaderForInterruptEndPoint can
        // also fail with STATUS_UNKNOWN_REVISION for an unsupported
        // tp_type - structural, not transient, so restart is requested
        // only for the resource-exhaustion case.
        if (status == STATUS_INSUFFICIENT_RESOURCES) {
            WdfDeviceSetFailed(Device, WdfDeviceFailedAttemptRestart);
        }
        return status;
    }

    pDeviceContext->PtpReportButton = TRUE;
    pDeviceContext->PtpReportTouch  = TRUE;

    AmtTrace(pDeviceContext, "PrepareHardware: EXIT OK, InterruptPipe=%p",
        pDeviceContext->InterruptPipe);

    return status;
}

// ---------------------------------------------------------------------
// RECOVERY/LIFECYCLE D0ExitLock HELPERS
// ---------------------------------------------------------------------
// See the prototype comments in Device.h for what each of these does and
// does NOT cover. Pure extraction of critical sections that were byte-
// identical across 2+ call sites in this file and Interrupt.c - no change
// to locking order, IRQL contract, or field semantics.

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtPtpRecoveryBeginTermination(
    _In_ PDEVICE_CONTEXT DeviceContext)
{
    WdfSpinLockAcquire(DeviceContext->D0ExitLock);
    DeviceContext->D0ExitInProgress = TRUE;
    DeviceContext->RecoveryGeneration++;
    WdfSpinLockRelease(DeviceContext->D0ExitLock);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
AmtPtpRecoveryMarkExhaustedIfCurrent(
    _In_ PDEVICE_CONTEXT DeviceContext,
    _In_ ULONG           SnapshotGeneration)
{
    BOOLEAN current;

    WdfSpinLockAcquire(DeviceContext->D0ExitLock);
    current = (DeviceContext->RecoveryGeneration == SnapshotGeneration);
    if (current) {
        DeviceContext->ReaderRecoveryStage = READER_RECOVERY_EXHAUSTED;
    }
    WdfSpinLockRelease(DeviceContext->D0ExitLock);

    return current;
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
    // NOTE: deliberately no PAGED_CODE() here - this function is no longer
    // in the PAGE section (see the alloc_text comment near the top of this
    // file) precisely because it acquires D0ExitLock (a WDFSPINLOCK)
    // below, which briefly raises IRQL to DISPATCH_LEVEL. Calling
    // PAGED_CODE() while genuinely resident is harmless by itself, but
    // pairing it with code that's NOT alloc_text(PAGE, ...) re-triggers
    // the exact PREfast C28172 inconsistency the removed pragma used to
    // paper over - so both are gone together. WdfWaitLockAcquire below
    // still enforces the real PASSIVE_LEVEL-only runtime contract on its
    // own (it is documented undefined behavior to call it above
    // PASSIVE_LEVEL), which is what actually mattered here.

    pDeviceContext = DeviceGetContext(Device);
    // See the identical PREfast note in AmtPtpDeviceUsbKmEvtDevicePrepareHardware.
    _Analysis_assume_(pDeviceContext != NULL);

    // DIAG: if this fires around a sleep/wake cycle (instead of only
    // around real PnP rebalance/removal), it means the stack is treating
    // resume as a surprise-removal + re-enumeration rather than an
    // ordinary D0Exit/D0Entry pair - that would explain a Code 10 that
    // AmtPtpEvtDeviceD0Entry's own logging can't otherwise account for,
    // since PrepareHardware would then need to re-run from scratch (fresh
    // UsbDevice/pipe/interface) instead of D0Entry reusing the existing
    // InterruptPipe handle.
    AmtTrace(pDeviceContext, "ReleaseHardware: ENTER, UsbDevice=%p InterruptPipe=%p",
        pDeviceContext->UsbDevice, pDeviceContext->InterruptPipe);

    // Step 1: short, DISPATCH-safe state transition. Mark lifecycle
    // terminating and bump RecoveryGeneration *before* touching anything
    // else, so any DISPATCH_LEVEL AmtPtpEvtUsbInterruptReadersFailed or a
    // PASSIVE_LEVEL AmtPtpEvtReaderRestartTimer that is only now taking
    // D0ExitLock sees this and backs off, and any timer callback that
    // already snapshotted an older generation notices the mismatch on its
    // post-RecoveryLock re-check (see AmtPtpEvtReaderRestartTimer).
    AmtPtpRecoveryBeginTermination(pDeviceContext);

    // Do not wait for the timer callback to finish (see the identical
    // reasoning in AmtPtpEvtDeviceD0Exit below) - it may be mid-flight in
    // an unbounded blocking USB call. RecoveryLock below is what actually
    // guarantees this function does not run concurrently with it.
    WdfTimerStop(pDeviceContext->ReaderRestartTimer, FALSE);

    // Step 2: serialize with any in-flight recovery/D0Exit cleanup at
    // PASSIVE_LEVEL. ReleaseHardware itself is documented by WDF as
    // PASSIVE_LEVEL-only (EVT_WDF_DEVICE_RELEASE_HARDWARE), so a blocking
    // wait here is sanctioned regardless of the PAGED_CODE()/alloc_text
    // question addressed above.
    WdfWaitLockAcquire(pDeviceContext->RecoveryLock, NULL);

    // Best-effort: stop the interrupt pipe's I/O target if it is still
    // running. Normally D0Exit already did this, but ReleaseHardware can
    // also be reached without an intervening D0Exit/D0Entry pair (e.g.
    // surprise removal), so this must not assume the target is already
    // stopped.
    if (pDeviceContext->InterruptPipe != NULL) {
        WdfIoTargetStop(
            WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe),
            WdfIoTargetCancelSentIo);
    }

    // Step 3: invalidate the USB handles under D0ExitLock so no concurrent
    // DISPATCH_LEVEL reader of InterruptPipe/UsbDevice/UsbInterface can
    // observe a torn/partial update. WdfUsbTargetDeviceCreate's returned
    // WDFUSBDEVICE, and the interface/pipe handles derived from it via
    // SelectInterruptInterface, are all parented to Device and torn down by
    // the framework automatically - this callback only needs to drop this
    // driver's own references to them so
    // AmtPtpDeviceUsbKmEvtDevicePrepareHardware's
    // "if (pDeviceContext->UsbDevice == NULL)" guard re-acquires a fresh
    // set on the next PrepareHardware instead of skipping acquisition and
    // reusing now-invalid handles. After this point, no recovery callback
    // may use these old USB objects - the generation bump in step 1 already
    // ensures no *new* recovery attempt will act on this D0 session, and
    // RecoveryLock ensures nothing is using them concurrently with this
    // clear.
    WdfSpinLockAcquire(pDeviceContext->D0ExitLock);
    pDeviceContext->InterruptPipe = NULL;
    pDeviceContext->UsbInterface  = NULL;
    pDeviceContext->UsbDevice     = NULL;
    WdfSpinLockRelease(pDeviceContext->D0ExitLock);

    WdfWaitLockRelease(pDeviceContext->RecoveryLock);

    return STATUS_SUCCESS;
}

// Attempt callback shape for the shared D0Entry retry helper, named in the
// same EVT_WDF_*-style convention WDF itself uses for callback typedefs
// (see e.g. EVT_WDF_DEVICE_D0_ENTRY in wdfdevice.h) even though this one
// is private to this file.
typedef NTSTATUS
(*PFN_AMT_D0ENTRY_ATTEMPT)(_In_ PDEVICE_CONTEXT DeviceContext);

// AmtPtpD0EntryRetry
//
// Shared bounded-retry-with-linear-backoff policy for a PASSIVE_LEVEL
// operation performed from EvtDeviceD0Entry.
//
// D3Final uses the existing full retry budget because a complete power-off
// can leave the USB device unsettled while the parent hub finishes bringing
// it back. A lighter D3 -> D0 resume gets a smaller, separate retry budget:
// on the affected T2 hardware WDF can enter D0 before the USB child has
// finished re-enumerating, so the first request can legitimately return
// STATUS_NO_SUCH_DEVICE.
//
// It can also legitimately return STATUS_INSUFFICIENT_RESOURCES: per
// Microsoft's Driver Verifier "Low Resources Simulation" documentation
// (learn.microsoft.com/windows-hardware/drivers/devtest/low-resources-simulation),
// this deliberately fails "random instances of the driver's memory
// allocations, as might occur if the driver was running on a computer with
// insufficient memory [...] to test the driver's ability to respond
// properly" - and the WDFREQUEST/WDFMEMORY allocations underlying both
// AmtPtpSetWellspringMode's control transfer and the continuous reader's
// resubmission are exactly the kind of allocation that option targets. A
// resume-window allocation failure is transient in exactly the same sense
// STATUS_NO_SUCH_DEVICE is (retrying a moment later is expected to
// succeed), so both statuses share this same bounded retry, confirmed
// against a real ~45s stall in SAKURAMBPRO.log driven by Verifier's "Pool
// Allocations Failed Deliberately" fault injection landing in this window.
// All other failures remain immediate failures and preserve the normal
// error path.
//
// Factored out so both D0Entry operations (SetWellspringMode and
// WdfIoTargetStart) share exactly the same bounded policy instead of
// carrying separate retry loops.
static BOOLEAN
AmtPtpIsTransientD0EntryStatus(_In_ NTSTATUS Status)
{
    return (Status == STATUS_NO_SUCH_DEVICE) ||
           (Status == STATUS_INSUFFICIENT_RESOURCES);
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
AmtPtpD0EntryRetry(
    _In_ PDEVICE_CONTEXT           DeviceContext,
    _In_ ULONG                     MaxAttempts,
    _In_ ULONG                     RetryDelayMsUnit,
    _In_ BOOLEAN                   RetryOnlyTransient,
    _In_ PCSTR                     OperationName,
    _In_ PFN_AMT_D0ENTRY_ATTEMPT   Attempt
    )
{
    ULONG    attempt;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    for (attempt = 0; attempt < MaxAttempts; attempt++) {
        status = Attempt(DeviceContext);
        AmtTrace(DeviceContext, "D0Entry: %s attempt %lu/%lu -> status=0x%08X",
            OperationName, attempt + 1, MaxAttempts, status);

        if (NT_SUCCESS(status)) {
            break;
        }

        if (RetryOnlyTransient && !AmtPtpIsTransientD0EntryStatus(status)) {
            break;
        }

        if (attempt + 1 < MaxAttempts) {
            LARGE_INTEGER delay;
            // D0Entry runs at PASSIVE_LEVEL, so a short blocking pause
            // here is sanctioned by the WDF power-callback contract -
            // this is not a hot path.
            delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(RetryDelayMsUnit * (attempt + 1));
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
    }

    return status;
}

//  - it calls through a
// PDEVICE_CONTEXT -> NTSTATUS function pointer so both D0Entry operations
// share one loop; these adapt each operation's real signature to that
// shape.

static NTSTATUS
AmtPtpD0EntrySetWellspringModeAttempt(_In_ PDEVICE_CONTEXT DeviceContext)
{
    return AmtPtpSetWellspringMode(DeviceContext, TRUE);
}

static NTSTATUS
AmtPtpD0EntryStartInterruptPipeAttempt(_In_ PDEVICE_CONTEXT DeviceContext)
{
    return WdfIoTargetStart(
        WdfUsbTargetPipeGetIoTarget(DeviceContext->InterruptPipe));
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
    // See the identical PREfast note in AmtPtpDeviceUsbKmEvtDevicePrepareHardware.
    _Analysis_assume_(pDeviceContext != NULL);
    isTargetStarted = FALSE;

    // DIAG: sleep/wake normally lands here with PreviousState D3 or
    // D3Final - if it's neither, D0Entry isn't even being reached the way
    // we assume.
    AmtTrace(pDeviceContext, "D0Entry: ENTER, PreviousState=%s, InterruptPipe=%p",
        DbgDevicePowerString(PreviousState), pDeviceContext->InterruptPipe);

    // Establish a fresh lifecycle generation before anything else runs.
    // RecoveryLock is taken first so this cannot interleave with a
    // still-finishing D0Exit/ReleaseHardware cleanup (which also holds
    // RecoveryLock across its own D0ExitLock-protected state transition) -
    // that ordering is what guarantees an old-generation timer callback can
    // never observe the new generation's state as if it were its own. A
    // fresh D0Entry always means a clean slate for the reader-recovery
    // escalation ladder in Interrupt.c - whatever failure streak happened
    // in a previous power session is irrelevant now.
    //
    // BUG FIX: RecoveryLock is now held across this entire function's
    // startup sequence (SetWellspringMode retries, the interrupt pipe's
    // WdfIoTargetStart retries, and the failure-path WdfIoTargetStop
    // below), not just the initial state bump. The two-level
    // synchronization model (see Device.h) documents RecoveryLock as
    // guaranteeing "at most one of { WdfIoTargetStop, ...,
    // WdfIoTargetStart } sequence is ever in flight at a time" - D0Entry's
    // own WdfIoTargetStart is one of those calls, and releasing
    // RecoveryLock right after the generation bump left it running
    // unserialized against AmtPtpEvtReaderRestartTimer/
    // AmtPtpEvtDeviceReleaseHardware, exactly the class of race this whole
    // lifecycle redesign exists to close. D0Entry runs at PASSIVE_LEVEL
    // (documented WDF contract for EvtDeviceD0Entry), so holding a
    // WDFWAITLOCK across these blocking calls is sanctioned - identical to
    // how AmtPtpEvtDeviceD0Exit already holds RecoveryLock across its own
    // WdfIoTargetStop/SetWellspringMode(FALSE) sequence.
    WdfWaitLockAcquire(pDeviceContext->RecoveryLock, NULL);
    WdfSpinLockAcquire(pDeviceContext->D0ExitLock);
    pDeviceContext->RecoveryGeneration++;
    pDeviceContext->D0ExitInProgress    = FALSE;
    pDeviceContext->ReaderRecoveryStage = READER_RECOVERY_RESET_PIPE;
    WdfSpinLockRelease(pDeviceContext->D0ExitLock);

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

    // Wellspring-mode setup is best-effort; only the interrupt-pipe start
    // result below determines this function's return value.
    //
    // D3Final keeps the full recovery retry budget. A normal D3 -> D0 resume
    // gets a smaller budget because the USB child may still be
    // re-enumerating on the parent hub when WDF calls D0Entry. In that case
    // retry only the transient statuses (STATUS_NO_SUCH_DEVICE and
    // STATUS_INSUFFICIENT_RESOURCES - see AmtPtpIsTransientD0EntryStatus)
    // so unrelated failures still take the normal error path.
    {
        const BOOLEAN fromD3Final =
            (PreviousState == WdfPowerDeviceD3Final);

        const ULONG maxAttempts = fromD3Final
            ? WELLSPRING_MODE_D0ENTRY_MAX_ATTEMPTS
            : WELLSPRING_MODE_D0ENTRY_RESUME_MAX_ATTEMPTS;

        const ULONG retryDelayMsUnit = fromD3Final
            ? WELLSPRING_MODE_D0ENTRY_RETRY_DELAY_MS_UNIT
            : WELLSPRING_MODE_D0ENTRY_RESUME_RETRY_DELAY_MS_UNIT;

        (VOID)AmtPtpD0EntryRetry(
            pDeviceContext,
            maxAttempts,
            retryDelayMsUnit,
            !fromD3Final,
            "SetWellspringMode",
            AmtPtpD0EntrySetWellspringModeAttempt);
    }

    // Starting the interrupt pipe's I/O target is subject to the same
    // post-resume USB settling window as SetWellspringMode above, and its
    // result IS this function's return value.
    {
        const BOOLEAN fromD3Final =
            (PreviousState == WdfPowerDeviceD3Final);

        const ULONG maxAttempts = fromD3Final
            ? INTERRUPT_PIPE_D0ENTRY_MAX_ATTEMPTS
            : INTERRUPT_PIPE_D0ENTRY_RESUME_MAX_ATTEMPTS;

        const ULONG retryDelayMsUnit = fromD3Final
            ? INTERRUPT_PIPE_D0ENTRY_RETRY_DELAY_MS_UNIT
            : INTERRUPT_PIPE_D0ENTRY_RESUME_RETRY_DELAY_MS_UNIT;

        status = AmtPtpD0EntryRetry(
            pDeviceContext,
            maxAttempts,
            retryDelayMsUnit,
            !fromD3Final,
            "WdfIoTargetStart",
            AmtPtpD0EntryStartInterruptPipeAttempt);
    }

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

    WdfWaitLockRelease(pDeviceContext->RecoveryLock);

    AmtTrace(pDeviceContext, "D0Entry: EXIT, status=0x%08X", status);

    return status;
}

// AmtPtpEvtDeviceD0Exit

NTSTATUS
AmtPtpEvtDeviceD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT pDeviceContext;
    NTSTATUS        wellspringOffStatus;

    // NOTE: deliberately no PAGED_CODE() here - see the identical note in
    // AmtPtpEvtDeviceReleaseHardware above (this function also acquires
    // D0ExitLock, a WDFSPINLOCK, below). WdfWaitLockAcquire still enforces
    // the real PASSIVE_LEVEL-only contract at runtime on its own.

    pDeviceContext = DeviceGetContext(Device);
    // See the identical PREfast note in AmtPtpDeviceUsbKmEvtDevicePrepareHardware.
    _Analysis_assume_(pDeviceContext != NULL);

    // DIAG: TargetState is now used below instead of discarded via
    // UNREFERENCED_PARAMETER, so sleep-wake cycles can be told apart from a
    // full power-off in the log.
    AmtTrace(pDeviceContext, "D0Exit: ENTER, TargetState=%s", DbgDevicePowerString(TargetState));

    // Step 1: short, DISPATCH-safe state transition (see the two-level
    // synchronization model comment in Device.h). Set this before touching
    // anything else, so AmtPtpEvtReaderRestartTimer/
    // AmtPtpEvtUsbInterruptReadersFailed - which can genuinely still be
    // mid-flight at this exact moment - notice and back off instead of
    // racing the WdfIoTargetStop call below on the same InterruptPipe.
    // Root-caused a real Bug Check 0x10D (WDF_VIOLATION, Parameter1=0x5,
    // Parameter2=0x0 - a NULL handle reaching a framework object method)
    // during a sleep transition. Bumping RecoveryGeneration here as well
    // means a timer callback that snapshotted the *previous* generation
    // before this point will fail its post-RecoveryLock re-check below even
    // if D0Entry has, by then, already cleared D0ExitInProgress again for a
    // new session. The lock acquisition itself is a short, non-blocking
    // spin (never held across a blocking call).
    AmtPtpRecoveryBeginTermination(pDeviceContext);

    // Do NOT wait here (Wait = FALSE). AmtPtpEvtReaderRestartTimer can be
    // mid-flight in WdfUsbTargetDeviceResetPortSynchronously, which per its
    // documented signature takes no WDFREQUEST/WDF_REQUEST_SEND_OPTIONS
    // and therefore has no time-out - if the port is in a bad enough state
    // to need that call, it can also be in a bad enough state for the call
    // to never return. WdfTimerStop(..., TRUE) would then block this
    // PASSIVE_LEVEL D0Exit callback indefinitely, which is exactly Bug
    // Check 0x9F (DRIVER_POWER_STATE_FAILURE), reason 0x3: "A device
    // object has been blocking an IRP for too long a time" - the power
    // manager times out waiting for this callback and Windows bugchecks,
    // which is unrecoverable short of a hard reboot.
    //
    // RecoveryLock below - not this call - is what actually guarantees no
    // recovery step is concurrently touching the pipe once this function
    // proceeds: a late timer callback either hasn't reached RecoveryLock
    // yet (and will fail its re-check once it does) or is already inside
    // RecoveryLock, in which case this function's own RecoveryLock
    // acquisition below simply waits its turn.
    WdfTimerStop(pDeviceContext->ReaderRestartTimer, FALSE);

    // Step 2: serialize the actual cleanup with any in-flight recovery at
    // PASSIVE_LEVEL. D0Exit itself is documented by WDF as PASSIVE_LEVEL-
    // only (EVT_WDF_DEVICE_D0_EXIT), so a blocking wait here is sanctioned
    // by the WDF power-callback contract regardless of the PAGED_CODE()/
    // alloc_text question addressed at the top of this function.
    WdfWaitLockAcquire(pDeviceContext->RecoveryLock, NULL);

    // Step 3: re-snapshot InterruptPipe under D0ExitLock now that
    // RecoveryLock is held. InterruptPipe is ordinarily non-NULL here (only
    // AmtPtpEvtDeviceReleaseHardware nulls it), but this driver has
    // observed physical device loss during S3 (STATUS_NO_SUCH_DEVICE in the
    // reader-recovery ladder). Skipping the stop when the pipe is already
    // gone is strictly safer than passing a NULL handle into
    // WdfUsbTargetPipeGetIoTarget.
    {
        WDFUSBPIPE snapshotPipe;

        WdfSpinLockAcquire(pDeviceContext->D0ExitLock);
        snapshotPipe = pDeviceContext->InterruptPipe;
        WdfSpinLockRelease(pDeviceContext->D0ExitLock);

        if (snapshotPipe != NULL) {
            WdfIoTargetStop(
                WdfUsbTargetPipeGetIoTarget(snapshotPipe),
                WdfIoTargetCancelSentIo);
        }
    }

    wellspringOffStatus = AmtPtpSetWellspringMode(pDeviceContext, FALSE);

    WdfWaitLockRelease(pDeviceContext->RecoveryLock);

    AmtTrace(pDeviceContext, "D0Exit: EXIT, SetWellspringMode(FALSE) -> status=0x%08X",
        wellspringOffStatus);

    return STATUS_SUCCESS;
}

// AmtPtpEvtDeviceContextCleanup
// Frees the manually-aligned ActiveContacts pool allocated in
// AmtPtpDeviceUsbKmCreateDevice, and detaches this FDO from the shared
// GUI-facing control device.

VOID
AmtPtpEvtDeviceContextCleanup(
    _In_ WDFOBJECT Device)
{
    WDFDEVICE        fdo = (WDFDEVICE)Device;
    PDEVICE_CONTEXT  pDeviceContext;
    PDRIVER_CONTEXT  driverContext;

    PAGED_CODE();

    pDeviceContext = DeviceGetContext(fdo);

    // The control device (DRIVER_CONTEXT::ConfigControlDevice) is
    // driver-lifetime, not FDO-lifetime - see AmtPtpAcquireConfigControlDevice
    // for the full rationale. This routine must therefore never delete it;
    // deletion happens exactly once, at driver unload, in
    // AmtPtpDeviceUsbKmEvtDriverContextCleanup (Driver.c). All this FDO
    // does on its own teardown is stop being the control device's current
    // target, under the same lock AmtPtpAcquireConfigControlDevice uses, so
    // the two can never interleave.
    //
    // The TargetDevice == fdo check matters: on a fast surprise-removal/
    // re-enumeration, the replacement FDO's EvtDeviceAdd can already have
    // called AmtPtpAcquireConfigControlDevice and re-pointed TargetDevice
    // at the NEW FDO before this (the OLD FDO's) cleanup gets to run.
    // Clearing unconditionally here would then null out the new FDO's live
    // pointer instead of this stale one, leaving the GUI's IOCTLs failing
    // as if no device were attached even though one legitimately is.
    driverContext = DriverGetContext(WdfDeviceGetDriver(fdo));

    WdfWaitLockAcquire(driverContext->ConfigControlDeviceLock, NULL);

    if (driverContext->ConfigControlDevice != NULL) {
        PAMT_CONFIG_CONTROL_CONTEXT controlContext =
            AmtConfigControlGetContext(driverContext->ConfigControlDevice);

        if (controlContext->TargetDevice == fdo) {
            controlContext->TargetDevice = NULL;
        }
    }

    WdfWaitLockRelease(driverContext->ConfigControlDeviceLock);

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