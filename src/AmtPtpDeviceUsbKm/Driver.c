// Driver entry points and callbacks. Kernel-mode Driver Framework

#define INITGUID
#include <initguid.h>
#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDeviceAdd)
#pragma alloc_text (PAGE, AmtPtpDeviceUsbKmEvtDriverContextCleanup)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
// Initialize the driver and register the device callback.
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    // Register the driver cleanup callback.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = AmtPtpDeviceUsbKmEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config,
                           AmtPtpDeviceUsbKmEvtDeviceAdd
                           );

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &config,
                             WDF_NO_HANDLE
                             );

    if (!NT_SUCCESS(status)) {
        return status;
    }
    return status;
}

NTSTATUS
AmtPtpDeviceUsbKmEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
// Create and initialize the device on add.
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    WdfFdoInitSetFilter(DeviceInit);
    WdfPdoInitAllowForwardingRequestToParent(DeviceInit);

    // Reverted: an earlier pass here called
    // WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, TRUE), on the
    // premise that nothing else in the stack was a real power-policy
    // owner. That premise was never actually verified against the live
    // device stack (Device Manager -> "Devices by connection"), and per
    // the WDF docs only one driver in a stack may be power-policy owner -
    // if something else (the underlying USB function driver, or a HID
    // class-extension-backed device) already holds it, a second claim
    // here is an invalid/unsupported dual-owner configuration, which is a
    // plausible cause of the STATUS_DEVICE_DATA_ERROR seen after this
    // change went in. WdfFdoInitSetFilter's default (NOT the power-policy
    // owner) is deliberately left in place until stack ownership is
    // confirmed with certainty.

    status = AmtPtpDeviceUsbKmCreateDevice(DeviceInit);

    if (!NT_SUCCESS(status)) {
    } else {
    }

    return status;
}

VOID
AmtPtpDeviceUsbKmEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
    )
// Release resources allocated in DriverEntry.
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE ();

}