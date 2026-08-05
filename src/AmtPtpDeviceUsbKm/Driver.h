// Driver definitions. Kernel-mode Driver Framework

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include "device.h"
#include "queue.h"
#include "trace.h"

#include <Hid.h>

EXTERN_C_START

// WDF driver events.

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AmtPtpDeviceUsbKmEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AmtPtpDeviceUsbKmEvtDriverContextCleanup;

// DIAGNOSTIC (always compiled, not #if DBG-gated): lets WinDbg read the
// real runtime alignment of DEVICE_CONTEXT::ActiveContacts via Local
// Kernel Debugging on an ordinary retail boot - no /debug boot flag, no
// checked/DBG driver build needed. Populated once in AmtPtpEvtDeviceD0Entry.
// See Device.c for the write site.
//
//   windbg -kl                                          (or Attach to kernel -> Local)
//   dt AmtPtpDeviceUsbKm!g_ActiveContactsAddress
//   dt AmtPtpDeviceUsbKm!g_ActiveContactsAlignOffset     (0 = 64B-aligned; nonzero = not)
extern ULONG_PTR g_ActiveContactsAddress;
extern ULONG     g_ActiveContactsAlignOffset;

EXTERN_C_END