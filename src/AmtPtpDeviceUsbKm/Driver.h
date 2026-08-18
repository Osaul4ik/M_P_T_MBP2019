// Driver definitions. Kernel-mode Driver Framework

#include <ntddk.h>
#include <wdf.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdfusb.h>
#include <usbioctl.h> // IOCTL_INTERNAL_USB_CYCLE_PORT - last rung of the
                       // reader-recovery escalation ladder (Interrupt.c)
#include "device.h"
#include "queue.h"

#include <Hid.h>

EXTERN_C_START

// Shared small geometry/decoding helpers. Every one of these used to be
// re-derived inline at each call site (ActiveContact.c, Match.c, Ptpcore.c,
// Input.c, Palm.c) - centralized here since Driver.h is already the first
// include in every translation unit.

// Absolute difference of two already-widened signed values. Replaces the
// repeated "INT d = a - b; if (d < 0) d = -d;" idiom used by every
// coordinate-delta / threshold check in the contact-matching pipeline.
static __forceinline INT
AmtAbsDelta(_In_ INT A, _In_ INT B)
{
    INT d = A - B;
    return (d < 0) ? -d : d;
}

// Squared Euclidean distance from two signed deltas. Replaces the repeated
// "(LONG)dx * dx + (LONG)dy * dy" used by every nearest-candidate /
// tie-break search (Match.c, Ptpcore.c).
static __forceinline ULONGLONG
AmtDistSq(_In_ INT Dx, _In_ INT Dy)
{
    LONGLONG x = (LONGLONG)Dx;
    LONGLONG y = (LONGLONG)Dy;
    return (ULONGLONG)(x * x + y * y);
}

// Raw HID field -> signed integer. Firmware reports touch_major/minor,
// pressure, and abs_x/abs_y as USHORT-encoded signed values; this was
// previously defined twice, byte-for-byte identically, as
// AmtInputRawToInteger (Input.c) and AmtPalmRawToInteger (Palm.c).
static __forceinline INT
AmtRawToSignedInt(_In_ USHORT x)
{
    return (signed short)x;
}

// WDF driver events.

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AmtPtpDeviceUsbKmEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP AmtPtpDeviceUsbKmEvtDriverContextCleanup;

EXTERN_C_END