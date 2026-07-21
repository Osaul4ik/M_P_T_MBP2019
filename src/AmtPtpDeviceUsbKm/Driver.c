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
// Initialises WPP tracing and registers EvtDevice callback.
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES attributes;

    // Init WPP
    WPP_INIT_TRACING( DriverObject, RegistryPath );

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
        WPP_CLEANUP(DriverObject);
        return status;
    }

    return status;
}

NTSTATUS
AmtPtpDeviceUsbKmEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
// Called by framework on AddDevice. Creates and initialises device.
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    WdfFdoInitSetFilter(DeviceInit);
    WdfPdoInitAllowForwardingRequestToParent(DeviceInit);

    status = AmtPtpDeviceUsbKmCreateDevice(DeviceInit);

    return status;
}

VOID
AmtPtpDeviceUsbKmEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
    )
// Frees resources allocated in DriverEntry.
{
    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE ();

    // Stop WPP
    WPP_CLEANUP( WdfDriverWdmGetDriverObject( (WDFDRIVER) DriverObject) );

}