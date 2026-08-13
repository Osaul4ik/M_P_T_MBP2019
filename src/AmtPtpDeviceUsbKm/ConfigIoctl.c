// ConfigIoctl.c - AmtPtpConfigGui <-> driver custom IOCTL surface.
//
// This is the kernel side of the "Elan-style" palm-rejection control panel.
// Every entry point here is reachable from user mode via
// IOCTL_AMT_PTP_{GET,SET}_PALM_CONFIG / GET_PAD_GEOMETRY / RESET_PALM_CONFIG
// (Queue.c dispatches EvtIoDeviceControl - the EXTERNAL IOCTL path - to
// these functions; see Queue.c for the dispatch switch). All of them are
// METHOD_BUFFERED, so WdfRequestRetrieveInputBuffer/OutputBuffer already
// give us a kernel-side copy - no direct user pointers touched here.

#include "Driver.h"
#include "ConfigIoctl.h"

// ----------------------------------------------------------------------------
// Clamp helper - shared by the SET IOCTL and the registry loader, so a
// corrupt registry value can never do anything worse than "silently
// clamped back into range", never a driver bugcheck or a degenerate
// classifier state (e.g. an edge zone that swallows the whole pad, or a
// PalmMinMajor above PalmLargeMajor).
// ----------------------------------------------------------------------------

static ULONG
AmtClampULong(_In_ ULONG Value, _In_ ULONG Min, _In_ ULONG Max)
{
    if (Value < Min) return Min;
    if (Value > Max) return Max;
    return Value;
}

VOID
AmtPalmConfigClamp(_Inout_ PAMT_PALM_CONFIG Config)
{
    Config->StructVersion = AMT_PALM_CONFIG_VERSION;

    Config->EdgePermilleTop    = AmtClampULong(Config->EdgePermilleTop,    0, AMT_PALM_EDGE_PERMILLE_MAX);
    Config->EdgePermilleLeft   = AmtClampULong(Config->EdgePermilleLeft,   0, AMT_PALM_EDGE_PERMILLE_MAX);
    Config->EdgePermilleRight  = AmtClampULong(Config->EdgePermilleRight, 0, AMT_PALM_EDGE_PERMILLE_MAX);
    Config->EdgePermilleBottom = AmtClampULong(Config->EdgePermilleBottom,0, AMT_PALM_EDGE_PERMILLE_MAX);

    Config->PalmLargeMajor  = AmtClampULong(Config->PalmLargeMajor,  1, AMT_PALM_MAJOR_MAX);
    Config->PalmLargeRatio  = AmtClampULong(Config->PalmLargeRatio,  1, AMT_PALM_RATIO_MAX);
    Config->PalmScoreThresh = AmtClampULong(Config->PalmScoreThresh,1, AMT_PALM_SCORE_MAX);
    Config->PalmMinMajor    = AmtClampULong(Config->PalmMinMajor,   0, AMT_PALM_MAJOR_MAX);
    Config->PalmMinMinor    = AmtClampULong(Config->PalmMinMinor,   0, AMT_PALM_MAJOR_MAX);

    RtlZeroMemory(Config->Reserved, sizeof(Config->Reserved));
}

// ----------------------------------------------------------------------------
// Registry persistence. Best-effort only: any failure (fresh install with
// no saved values yet, access-denied, corrupt/missing value) just leaves
// whatever the caller already had in *Config - which the caller has always
// pre-seeded with AMT_PALM_CONFIG_DEFAULT_INIT before calling Load.
// ----------------------------------------------------------------------------

static VOID
AmtRegistryReadDword(
    _In_ WDFKEY Key,
    _In_ PCWSTR ValueName,
    _Inout_ PULONG Value
)
{
    UNICODE_STRING valueName;
    ULONG          readValue = 0;
    NTSTATUS       status;

    RtlInitUnicodeString(&valueName, ValueName);
    status = WdfRegistryQueryULong(Key, &valueName, &readValue);
    if (NT_SUCCESS(status)) {
        *Value = readValue;
    }
    // else: leave *Value (the pre-seeded default) untouched.
}

static VOID
AmtRegistryWriteDword(
    _In_ WDFKEY Key,
    _In_ PCWSTR ValueName,
    _In_ ULONG Value
)
{
    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, ValueName);
    // Best-effort: ignore failure (e.g. read-only key on a locked-down
    // system) - the in-memory PalmConfig is already updated by the caller
    // regardless, so classification behavior is correct even if the save
    // silently doesn't stick across reboot.
    (VOID)WdfRegistryAssignULong(Key, &valueName, Value);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
AmtPalmConfigLoadFromRegistry(
    _In_ WDFDEVICE Device,
    _Inout_ PAMT_PALM_CONFIG Config
)
{
    WDFKEY   key;
    NTSTATUS status;

    PAGED_CODE();

    status = WdfDeviceOpenRegistryKey(
        Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) {
        return; // No saved values yet (or no access) - keep compiled-in defaults.
    }

    AmtRegistryReadDword(key, AMT_REG_VALUE_EDGE_TOP,     &Config->EdgePermilleTop);
    AmtRegistryReadDword(key, AMT_REG_VALUE_EDGE_LEFT,    &Config->EdgePermilleLeft);
    AmtRegistryReadDword(key, AMT_REG_VALUE_EDGE_RIGHT,   &Config->EdgePermilleRight);
    AmtRegistryReadDword(key, AMT_REG_VALUE_EDGE_BOTTOM,  &Config->EdgePermilleBottom);
    AmtRegistryReadDword(key, AMT_REG_VALUE_LARGE_MAJOR,  &Config->PalmLargeMajor);
    AmtRegistryReadDword(key, AMT_REG_VALUE_LARGE_RATIO,  &Config->PalmLargeRatio);
    AmtRegistryReadDword(key, AMT_REG_VALUE_SCORE_THRESH, &Config->PalmScoreThresh);
    AmtRegistryReadDword(key, AMT_REG_VALUE_MIN_MAJOR,    &Config->PalmMinMajor);
    AmtRegistryReadDword(key, AMT_REG_VALUE_MIN_MINOR,    &Config->PalmMinMinor);

    WdfRegistryClose(key);

    // A value loaded straight from the registry is untrusted input the
    // same as anything arriving over the SET IOCTL - clamp it too.
    AmtPalmConfigClamp(Config);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
AmtPalmConfigSaveToRegistry(
    _In_ WDFDEVICE Device,
    _In_ const AMT_PALM_CONFIG* Config
)
{
    WDFKEY   key;
    NTSTATUS status;

    // WdfDeviceOpenRegistryKey is PASSIVE_LEVEL-only; the SET IOCTL handler
    // below always runs at PASSIVE_LEVEL (default parallel WDFQUEUE, no
    // DISPATCH_LEVEL constraint set on it), so this is safe despite the
    // DISPATCH_LEVEL-max annotation on the prototype (kept loose so future
    // callers don't need to special-case IRQL).
    status = WdfDeviceOpenRegistryKey(
        Device, PLUGPLAY_REGKEY_DEVICE, KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) {
        return; // Best-effort - in-memory config is already applied regardless.
    }

    AmtRegistryWriteDword(key, AMT_REG_VALUE_EDGE_TOP,     Config->EdgePermilleTop);
    AmtRegistryWriteDword(key, AMT_REG_VALUE_EDGE_LEFT,    Config->EdgePermilleLeft);
    AmtRegistryWriteDword(key, AMT_REG_VALUE_EDGE_RIGHT,   Config->EdgePermilleRight);
    AmtRegistryWriteDword(key, AMT_REG_VALUE_EDGE_BOTTOM,  Config->EdgePermilleBottom);
    AmtRegistryWriteDword(key, AMT_REG_VALUE_LARGE_MAJOR,  Config->PalmLargeMajor);
    AmtRegistryWriteDword(key, AMT_REG_VALUE_LARGE_RATIO,  Config->PalmLargeRatio);
    AmtRegistryWriteDword(key, AMT_REG_VALUE_SCORE_THRESH, Config->PalmScoreThresh);
    AmtRegistryWriteDword(key, AMT_REG_VALUE_MIN_MAJOR,    Config->PalmMinMajor);
    AmtRegistryWriteDword(key, AMT_REG_VALUE_MIN_MINOR,    Config->PalmMinMinor);

    WdfRegistryClose(key);
}

// ----------------------------------------------------------------------------
// IOCTL handlers. The GUI reaches these through the separate KMDF control
// device created in Device.c. The dispatcher passes the real USB filter FDO
// as Device, so the existing DEVICE_CONTEXT/state/registry logic is retained.
// ----------------------------------------------------------------------------

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpGetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    NTSTATUS         status;
    PDEVICE_CONTEXT  pDeviceContext;
    PAMT_PALM_CONFIG pOutConfig;
    size_t           outLen = 0;

    pDeviceContext = DeviceGetContext(Device);

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(AMT_PALM_CONFIG), (PVOID*)&pOutConfig, &outLen);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    WdfSpinLockAcquire(pDeviceContext->StateLock);
    *pOutConfig = pDeviceContext->PalmConfig;
    WdfSpinLockRelease(pDeviceContext->StateLock);

    WdfRequestSetInformation(Request, sizeof(AMT_PALM_CONFIG));
    status = STATUS_SUCCESS;

exit:
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpSetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    NTSTATUS         status;
    PDEVICE_CONTEXT  pDeviceContext;
    PAMT_PALM_CONFIG pInConfig;
    size_t           inLen = 0;
    AMT_PALM_CONFIG  clamped;

    pDeviceContext = DeviceGetContext(Device);

    status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(AMT_PALM_CONFIG), (PVOID*)&pInConfig, &inLen);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    // Copy out of the (still user-influenced) buffer before validating/
    // applying, then clamp every field into a safe range - never trust the
    // GUI (or anything else calling this IOCTL) to have done that itself.
    clamped = *pInConfig;
    AmtPalmConfigClamp(&clamped);

    WdfSpinLockAcquire(pDeviceContext->StateLock);
    pDeviceContext->PalmConfig = clamped;
    WdfSpinLockRelease(pDeviceContext->StateLock);

    // Persist so the choice survives a reboot/replug, same as an Elan/
    // Synaptics OEM panel's "Apply" button. Best-effort - see
    // AmtPalmConfigSaveToRegistry.
    AmtPalmConfigSaveToRegistry(Device, &clamped);

    // Echo the clamped values back so the GUI can immediately reflect any
    // adjustment the driver made to an out-of-range input, instead of the
    // slider silently drifting from what's actually in effect.
    {
        PAMT_PALM_CONFIG pOutConfig;
        size_t           outLen = 0;
        if (NT_SUCCESS(WdfRequestRetrieveOutputBuffer(
                Request, sizeof(AMT_PALM_CONFIG), (PVOID*)&pOutConfig, &outLen))) {
            *pOutConfig = clamped;
            WdfRequestSetInformation(Request, sizeof(AMT_PALM_CONFIG));
        }
    }

    status = STATUS_SUCCESS;

exit:
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpGetPadGeometry(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    NTSTATUS          status;
    PDEVICE_CONTEXT   pDeviceContext;
    PAMT_PAD_GEOMETRY pOutGeometry;
    size_t            outLen = 0;

    pDeviceContext = DeviceGetContext(Device);

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(AMT_PAD_GEOMETRY), (PVOID*)&pOutGeometry, &outLen);
    if (!NT_SUCCESS(status)) {
        goto exit;
    }

    if (pDeviceContext->DeviceInfo == NULL) {
        // Hardware not enumerated yet (queried before EvtDevicePrepareHardware
        // finished, or device in a low-power/removed state) - report a
        // clearly-invalid-but-safe zero range instead of dereferencing NULL.
        // The GUI's preview mode treats XMax<=XMin as "use fallback pad size".
        RtlZeroMemory(pOutGeometry, sizeof(AMT_PAD_GEOMETRY));
        pOutGeometry->StructVersion = AMT_PAD_GEOMETRY_VERSION;
        status = STATUS_SUCCESS;
        goto exit;
    }

    pOutGeometry->StructVersion = AMT_PAD_GEOMETRY_VERSION;
    pOutGeometry->XMin = pDeviceContext->DeviceInfo->x.min;
    pOutGeometry->XMax = pDeviceContext->DeviceInfo->x.max;
    pOutGeometry->YMin = pDeviceContext->DeviceInfo->y.min;
    pOutGeometry->YMax = pDeviceContext->DeviceInfo->y.max;

    WdfRequestSetInformation(Request, sizeof(AMT_PAD_GEOMETRY));
    status = STATUS_SUCCESS;

exit:
    return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS
AmtPtpResetPalmConfig(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PDEVICE_CONTEXT pDeviceContext = DeviceGetContext(Device);
    AMT_PALM_CONFIG defaults = AMT_PALM_CONFIG_DEFAULT_INIT;

    UNREFERENCED_PARAMETER(Request); // no in/out buffer for this IOCTL

    WdfSpinLockAcquire(pDeviceContext->StateLock);
    pDeviceContext->PalmConfig = defaults;
    WdfSpinLockRelease(pDeviceContext->StateLock);

    AmtPalmConfigSaveToRegistry(Device, &defaults);

    return STATUS_SUCCESS;
}
