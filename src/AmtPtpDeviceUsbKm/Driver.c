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

    // CONFIRMED (Device Manager -> "Devices by connection" on real
    // hardware): Intel PCIe root -> Apple USB Virtual Host Controller ->
    // USB Root Hub (USB 3.0) -> USB Composite Device -> Wellspring
    // Precision Touchpad. The "Wellspring Precision Touchpad" node is the
    // PDO usbccgp (the USB Composite Device's own function driver) creates
    // for interface MI_02; mshidkmdf is the actual (non-filter) function
    // driver bound to that PDO per the KMDF HID-minidriver model, with
    // this driver sitting underneath it as LowerFilters (see the
    // AmtPtpDeviceUsbKm_AddReg LowerFilters entry in the INF). Per the WDF
    // docs, only a non-filter function driver auto-claims power-policy
    // ownership; WdfFdoInitSetFilter's default (this driver is NOT the
    // owner) is therefore not just the safe fallback but the architecturally
    // correct answer here - mshidkmdf, the real function driver one layer
    // above us, is the power-policy owner. A prior pass had briefly called
    // WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, TRUE) on the
    // unverified premise that nothing else owned it; that produced a
    // STATUS_DEVICE_DATA_ERROR (an invalid dual-owner configuration - only
    // one driver per stack may hold power-policy ownership), was reverted,
    // and must not be reinstated.

    status = AmtPtpDeviceUsbKmCreateDevice(DeviceInit);

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