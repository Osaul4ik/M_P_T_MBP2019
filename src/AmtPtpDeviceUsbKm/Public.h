// Public.h - Shared declarations for driver and user apps.
//
// This header is included by BOTH the kernel-mode driver and any user-mode
// configuration application (e.g. AmtPtpConfigGui). Keep it free of
// kernel-only or user-only types - only PODs that are safe to marshal
// across the DeviceIoControl boundary belong here.

#pragma once

// Device interface GUID for app communication.

DEFINE_GUID (GUID_DEVINTERFACE_AmtPtpDeviceUsbKm,
    0x4aa332cc,0x5777,0x4afd,0xaa,0x4e,0x95,0x38,0x73,0x30,0x61,0x2a);
// {4aa332cc-5777-4afd-aa4e-95387330612a}

// ============================================================================
// Palm-rejection runtime configuration - shared wire format.
//
// This mirrors (and, at runtime, replaces) the tuning constants that used
// to be hardcoded #defines in Palm.c. A user-mode control panel
// (AmtPtpConfigGui) can read/write this struct live via the custom IOCTLs
// below, the same way Elan's/Synaptics' OEM control panels expose palm
// rejection / edge-zone sliders.
//
// All fields are plain ULONG so the struct is trivially blittable from C#
// (Marshal.SizeOf / [StructLayout(LayoutKind.Sequential)]) with no padding
// surprises - every field is the same size, so no compiler will insert
// alignment padding between them on either x86 or x64.
// ============================================================================

typedef struct _AMT_PALM_CONFIG
{
    ULONG StructVersion;   // AMT_PALM_CONFIG_VERSION - bump on layout change

    // Edge-zone size, in PERMILLE (parts-per-1000, i.e. 0.1%) of the pad's
    // usable width/height. A birth (fresh finger) landing inside this zone
    // is hard-rejected as palm. See Palm.c / AmtPalmInEdgeZone.
    ULONG EdgePermilleTop;
    ULONG EdgePermilleLeft;
    ULONG EdgePermilleRight;
    ULONG EdgePermilleBottom;

    // Contact-shape thresholds (raw sensor units - same scale as the
    // Major/Minor axis values reported by the trackpad firmware).
    ULONG PalmLargeMajor;   // major >= this -> candidate for instant PALM_LARGE
    ULONG PalmLargeRatio;   // major/minor*100 >= this (with PalmLargeMajor) -> PALM_LARGE
    ULONG PalmScoreThresh;  // soft-score threshold -> PALM_LOCAL
    ULONG PalmMinMajor;     // below this AND PalmMinMinor -> never palm (early out)
    ULONG PalmMinMinor;

    ULONG Reserved[4];      // future growth, keep struct size stable
} AMT_PALM_CONFIG, *PAMT_PALM_CONFIG;

#define AMT_PALM_CONFIG_VERSION 1

// Compiled-in defaults - identical to the previous hardcoded values in
// Palm.c, so a fresh install / registry-less first boot behaves exactly
// like the driver did before this config became runtime-tunable.
#define AMT_PALM_CONFIG_DEFAULT_INIT                                        \
{                                                                            \
    /* StructVersion       */ AMT_PALM_CONFIG_VERSION,                      \
    /* EdgePermilleTop     */ 0,                                           \
    /* EdgePermilleLeft    */ 70,                                          \
    /* EdgePermilleRight   */ 70,                                          \
    /* EdgePermilleBottom  */ 110,                                          \
    /* PalmLargeMajor      */ 380,                                          \
    /* PalmLargeRatio      */ 180,                                          \
    /* PalmScoreThresh     */ 55,                                           \
    /* PalmMinMajor        */ 80,                                           \
    /* PalmMinMinor        */ 40,                                           \
    /* Reserved            */ { 0, 0, 0, 0 }                                \
}

// Sane clamp range for the GUI/driver to enforce on every field except
// StructVersion/Reserved - keeps a fat-fingered value from disabling palm
// rejection outright or rejecting every touch as palm.
#define AMT_PALM_EDGE_PERMILLE_MAX   400   // 40% - beyond this, "edge zone" eats the whole pad
#define AMT_PALM_MAJOR_MAX          2000
#define AMT_PALM_RATIO_MAX          1000
#define AMT_PALM_SCORE_MAX           200


// ============================================================================
// Pointer runtime configuration - Force Tap (force touch) tuning.
//
// Mirrors (and, at runtime, replaces) FORCE_TOUCH_PRESSURE_THRESHOLD, which
// used to be a hardcoded #define in include/hid/HidCommon.h, plus the
// synthetic action a qualifying press fires on release - previously always
// a hardcoded right-click (Button2) in Interrupt.c. Same wire-format
// conventions as AMT_PALM_CONFIG above (plain ULONGs, blittable from C#).
// ============================================================================

typedef struct _AMT_POINTER_CONFIG
{
    ULONG StructVersion;   // AMT_POINTER_CONFIG_VERSION - bump on layout change
    ULONG ForceTapThreshold;
    ULONG ForceTapAction;

    // Cursor motion tuning. Percent values use 100 as the current/default
    // behavior. CursorSmoothingPercent: 0 = raw, 100 = strongest smoothing.
    ULONG CursorSmoothingPercent;
    ULONG CursorSpeedPercent;
    ULONG CursorDeadzone;
    ULONG CursorDeadzoneSlow;
    ULONG CursorDeadzoneFast;
    ULONG CursorSlowVelocity;
    ULONG CursorFastVelocity;
    ULONG SmoothingAlphaDen;
    ULONG SmoothingAlphaNumSlow;
} AMT_POINTER_CONFIG, *PAMT_POINTER_CONFIG;

#define AMT_POINTER_CONFIG_VERSION 3

#define AMT_POINTER_SMOOTH_MIN       0
#define AMT_POINTER_SMOOTH_MAX       100
#define AMT_POINTER_SPEED_MIN        50
#define AMT_POINTER_SPEED_MAX        200
#define AMT_POINTER_DEADZONE_MIN     0
#define AMT_POINTER_DEADZONE_MAX     8
#define AMT_POINTER_SLOW_VEL_MIN     20
#define AMT_POINTER_SLOW_VEL_MAX     300
#define AMT_POINTER_FAST_VEL_MIN     200
#define AMT_POINTER_FAST_VEL_MAX     2000
#define AMT_POINTER_ALPHA_DEN_MIN    1
#define AMT_POINTER_ALPHA_DEN_MAX    16
#define AMT_POINTER_ALPHA_SLOW_MIN  1
#define AMT_POINTER_ALPHA_SLOW_MAX 16

// ForceTapAction values.
#define AMT_POINTER_ACTION_CONTEXT_MENU 0   // synthetic right-click (Button2)
#define AMT_POINTER_ACTION_MIDDLE_CLICK 1   // synthetic middle-click (Button3)
#define AMT_POINTER_ACTION_DOUBLE_CLICK 2   // synthetic double left-click (Button1 x2, e.g. "open")

// Compiled-in defaults - tuned factory values for the current Wellspring PTP
// experience. CONTEXT_MENU remains the default Force Tap action so a
// previous always-right-click behavior, so a fresh install / registry-less
// first boot behaves exactly like the driver did before this became
// runtime-tunable.
#define AMT_POINTER_CONFIG_DEFAULT_INIT                                    \
{                                                                            \
    AMT_POINTER_CONFIG_VERSION,                                             \
    250,                                                                    \
    AMT_POINTER_ACTION_CONTEXT_MENU,                                        \
    0,                                                                      \
    100,                                                                    \
    1,                                                                      \
    4,                                                                      \
    0,                                                                      \
    110,                                                                    \
    905,                                                                    \
    8,                                                                      \
    3                                                                       \
}

// Sane clamp range - raw pressure realistically spans ~0-300, so keep the
// threshold well inside that instead of letting it go degenerate (0 would
// fire on any touch; a huge value would make Force Tap unreachable).
#define AMT_POINTER_THRESHOLD_MIN   200
#define AMT_POINTER_THRESHOLD_MAX   400
#define AMT_POINTER_ACTION_MAX        2   // highest valid AMT_POINTER_ACTION_* value


// ============================================================================
// Live touch monitor
//
// Live monitoring is explicitly opt-in. When LiveEnabled == FALSE the
// interrupt hot path does not build/copy a live snapshot. The GUI enables
// it with IOCTL_AMT_PTP_SET_LIVE_ENABLED and polls the latest snapshot with
// IOCTL_AMT_PTP_GET_LIVE_FRAME.
// ============================================================================

#define AMT_LIVE_FRAME_VERSION 2
#define AMT_LIVE_MAX_CONTACTS 5

typedef struct _AMT_LIVE_CONTACT
{
    ULONG ContactID;
    USHORT X;              // normalized X: 0 .. (XMax - XMin)
    USHORT Y;              // normalized Y: 0 .. (YMax - YMin)
    ULONG Phase;           // CONTACT_PHASE_* value from PTPCore.h
    UCHAR Confident;
    UCHAR PalmSuspect;
    USHORT Reserved;
    SHORT RawX;            // exact raw USB abs_x
    SHORT RawY;            // exact raw USB abs_y
    USHORT Major;          // touch_major, raw sensor units (nearest-raw match)
    USHORT Minor;          // touch_minor, raw sensor units (nearest-raw match)
} AMT_LIVE_CONTACT, *PAMT_LIVE_CONTACT;

typedef struct _AMT_LIVE_FRAME
{
    ULONG StructVersion;
    ULONG Sequence;
    LONGLONG TimestampQpc;

    UCHAR ContactCount;
    UCHAR RawContactCount;
    UCHAR LargePalmBlanked;
    UCHAR ButtonDown;

    UCHAR ForceTouchClick;
    UCHAR ButtonClickReport;
    USHORT Reserved0;

    AMT_LIVE_CONTACT Contacts[AMT_LIVE_MAX_CONTACTS];
} AMT_LIVE_FRAME, *PAMT_LIVE_FRAME;

// ============================================================================
// Custom IOCTLs for AmtPtpConfigGui <-> driver communication.
//
// FILE_DEVICE_UNKNOWN + METHOD_BUFFERED + FILE_ANY_ACCESS: standard,
// conservative choice for a small buffered get/set pair - no direct
// pointers cross the user/kernel boundary, and any authenticated user can
// open the device interface (matches how the rest of this driver's HID
// surface is exposed).
// ============================================================================

#define AMT_PTP_IOCTL_INDEX 0x900

#define IOCTL_AMT_PTP_GET_PALM_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 0, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AMT_PTP_SET_PALM_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 1, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Returns the pad's usable sensor range (BCM5974_CONFIG x/y min/max) so the
// GUI can draw the edge zones and a to-scale finger ellipse without
// guessing at hardware geometry. Read-only.
#define IOCTL_AMT_PTP_GET_PAD_GEOMETRY \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 2, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Resets AMT_PALM_CONFIG (in memory and in the registry) to
// AMT_PALM_CONFIG_DEFAULT_INIT. No input/output buffer.
#define IOCTL_AMT_PTP_RESET_PALM_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 3, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AMT_PTP_SET_LIVE_ENABLED \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 4, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AMT_PTP_GET_LIVE_FRAME \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 5, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AMT_PTP_GET_POINTER_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 6, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AMT_PTP_SET_POINTER_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 7, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Resets AMT_POINTER_CONFIG (in memory and in the registry) to
// AMT_POINTER_CONFIG_DEFAULT_INIT. No input/output buffer.
#define IOCTL_AMT_PTP_RESET_POINTER_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 8, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AMT_PTP_GET_SCROLL_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 9, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AMT_PTP_SET_SCROLL_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 10, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_AMT_PTP_RESET_SCROLL_CONFIG \
    CTL_CODE(FILE_DEVICE_UNKNOWN, AMT_PTP_IOCTL_INDEX + 11, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _AMT_SCROLL_CONFIG
{
    ULONG StructVersion;
    ULONG SpeedPercent;
    ULONG FastSpeedPercent;
    ULONG SmoothingPercent;
    ULONG Deadzone;
    ULONG FastVelocity;
    ULONG ScaleNum;
    ULONG ScaleDen;
    ULONG ScaleNumFast;
    ULONG ScaleDenFast;
    ULONG Reserved[2];
} AMT_SCROLL_CONFIG, *PAMT_SCROLL_CONFIG;

#define AMT_SCROLL_CONFIG_VERSION 2
#define AMT_SCROLL_SPEED_MIN          20
#define AMT_SCROLL_SPEED_MAX          200
#define AMT_SCROLL_FAST_SPEED_MIN     20
#define AMT_SCROLL_FAST_SPEED_MAX     250
#define AMT_SCROLL_SMOOTH_MIN         0
#define AMT_SCROLL_SMOOTH_MAX         100
#define AMT_SCROLL_DEADZONE_MIN       0
#define AMT_SCROLL_DEADZONE_MAX       8
#define AMT_SCROLL_FAST_VEL_MIN       500
#define AMT_SCROLL_FAST_VEL_MAX       4000
#define AMT_SCROLL_SCALE_NUM_MIN       1
#define AMT_SCROLL_SCALE_NUM_MAX     400
#define AMT_SCROLL_SCALE_DEN_MIN       1
#define AMT_SCROLL_SCALE_DEN_MAX     400
#define AMT_SCROLL_CONFIG_DEFAULT_INIT \
{ \
    AMT_SCROLL_CONFIG_VERSION, \
    60, \
    100, \
    0, \
    1, \
    1600, \
    6, \
    10, \
    108, \
    100, \
    { 0, 0 } \
}

typedef struct _AMT_PAD_GEOMETRY
{
    ULONG StructVersion;
    LONG  XMin;
    LONG  XMax;
    LONG  YMin;
    LONG  YMax;
} AMT_PAD_GEOMETRY, *PAMT_PAD_GEOMETRY;

#define AMT_PAD_GEOMETRY_VERSION 1