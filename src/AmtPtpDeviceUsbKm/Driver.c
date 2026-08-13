// Driver entry points and callbacks. Kernel-mode Driver Framework

#define INITGUID
#include <initguid.h>
#include "driver.h"
#include "driver.tmh"

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
// Initialize tracing and register the device callback.
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    // Init WPP
    WPP_INIT_TRACING( DriverObject, RegistryPath );

    AMT_LOG("DriverEntry called - driver image loaded by PnP manager");

    // Register cleanup callback for WPP_CLEANUP.
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
        AMT_LOG("WdfDriverCreate FAILED, status=0x%08X", status);
        WPP_CLEANUP(DriverObject);
        return status;
    }

    AMT_LOG("WdfDriverCreate succeeded");
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

    AMT_LOG("EvtDeviceAdd called - PnP manager is adding a device instance for our hwid/compatid match");

    WdfFdoInitSetFilter(DeviceInit);
    WdfPdoInitAllowForwardingRequestToParent(DeviceInit);

    status = AmtPtpDeviceUsbKmCreateDevice(DeviceInit);

    if (!NT_SUCCESS(status)) {
        AMT_LOG("AmtPtpDeviceUsbKmCreateDevice FAILED, status=0x%08X - device will NOT start, "
                "no device interface will exist, GUI has nothing to find", status);
    } else {
        AMT_LOG("AmtPtpDeviceUsbKmCreateDevice succeeded - device interface should now be registered");
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

    // Stop WPP
    WPP_CLEANUP( WdfDriverWdmGetDriverObject( (WDFDRIVER) DriverObject) );

}