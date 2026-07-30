// Driver definitions. Kernel-mode Driver Framework

// TEMP DEBUG SWITCHES: each one independently bypasses a single "bolted-
// on" smoothing/heuristic layer added on top of the base contact-
// tracking pipeline, so each can be re-enabled and tuned on its own
// rather than all-or-nothing. Set a switch to 0 for the normal/shipping
// behavior of that layer, 1 to bypass it and expose the raw underlying
// behavior for diagnosis. None of these touch actual bug fixes to the
// base pipeline itself (the QPC/timestamp fix in Interrupt.c, core
// candidate matching) - only the extra layers built on top of it.
//
// Status (re-enabling one at a time, tuning against the current fixed
// pipeline before moving to the next):
//   [x] AMT_RAW_DISABLE_GESTURE_DEBOUNCE   - re-enabled (0)
//   [ ] AMT_RAW_DISABLE_RETAP_SMOOTHING    - still raw (1)
//   [ ] AMT_RAW_DISABLE_BUTTON_DEBOUNCE    - still raw (1)
//   [ ] AMT_RAW_DISABLE_IDENTITY_BREAK_FIX - still raw (1)
//   [ ] AMT_RAW_DISABLE_POSITION_SMOOTHING - still raw (1)
#define AMT_RAW_DISABLE_GESTURE_DEBOUNCE   0
#define AMT_RAW_DISABLE_RETAP_SMOOTHING    1
#define AMT_RAW_DISABLE_BUTTON_DEBOUNCE    1
#define AMT_RAW_DISABLE_IDENTITY_BREAK_FIX 1
#define AMT_RAW_DISABLE_POSITION_SMOOTHING 1

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