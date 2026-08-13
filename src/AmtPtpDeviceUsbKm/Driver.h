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

// AMT_LOG - unconditional diagnostic print, visible in DebugView (Capture
// Kernel) or a local/remote WinDbg session regardless of build config.
//
// Deliberately NOT KdPrint(): KdPrint() expands to nothing unless DBG is
// defined by the build (checked/Debug configuration only) - in Release and
// ReleaseSigned it's a silent no-op, which is why the previous single
// KdPrint in AmtPtpEvtDeviceFileCreate produced nothing when the driver was
// built/deployed as ReleaseSigned. DbgPrintEx has no such gate: it always
// compiles in and always emits, in every configuration.
//
// Also deliberately NOT the WPP TraceEvents() machinery wired up in
// Trace.h: WPP output only goes to an ETW trace session (TraceView.exe /
// tracelog+tracefmt), never to DebugView or a plain WinDbg "kd" prompt, and
// nothing in this driver actually calls TraceEvents() yet.
//
// DPFLTR_ERROR_LEVEL is used (not INFO/WARNING/TRACE) specifically because
// it is the one level DbgPrintEx always shows regardless of the
// "Debug Print Filter" component mask in the registry - no extra registry
// setup is required to see these in DebugView. Prefix every line with
// [AmtPtpUsbKm] so it's easy to filter in DebugView's Filter/Highlight box.
#define AMT_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
        "[AmtPtpUsbKm] %s: " fmt "\n", __FUNCTION__, ##__VA_ARGS__)

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
static __forceinline LONG
AmtDistSq(_In_ INT Dx, _In_ INT Dy)
{
    return (LONG)Dx * Dx + (LONG)Dy * Dy;
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