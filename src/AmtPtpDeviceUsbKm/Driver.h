// Driver definitions. Kernel-mode Driver Framework

// TEMP DEBUG SWITCH: set to 1 to bypass every "bolted-on" smoothing/
// heuristic layer added on top of the base contact-tracking pipeline
// (EMA/deadzone position smoothing, retap-seed EMA blending, gesture-
// taint debounce, firmware IdentityBreak jump-suppression, and the
// physical-click debounce) and expose the raw underlying behavior for
// diagnosis. This does NOT touch actual bug fixes to the base pipeline
// itself (the QPC/timestamp fix in Interrupt.c, core candidate matching)
// - only the extra layers built on top of it. Set back to 0 (the normal/
// shipping behavior) once the raw-mode comparison log has been captured.
#define AMT_RAW_DEBUG_MODE 1

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

// WDFDRIVER Events

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AmtPtpDeviceUsbKmEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AmtPtpDeviceUsbKmEvtDriverContextCleanup;

EXTERN_C_END