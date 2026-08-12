#pragma once

#define REPORTID_STANDARDMOUSE 0x02
#define REPORTID_MULTITOUCH 0x05
#define REPORTID_REPORTMODE 0x04
#define REPORTID_PTPHQA 0x08
#define REPORTID_FUNCSWITCH 0x06
#define REPORTID_DEVICE_CAPS 0x07
#define REPORTID_UMAPP_CONF  0x09

#define BUTTON_SWITCH 0x57
#define SURFACE_SWITCH 0x58

#define USAGE_PAGE 0x05
#define USAGE_PAGE_1 0x06
#define USAGE      0x09
#define USAGE_MINIMUM 0x19
#define USAGE_MAXIMUM 0x29
#define LOGICAL_MINIMUM 0x15
#define LOGICAL_MAXIMUM 0x25
#define LOGICAL_MAXIMUM_2 0x26
#define LOGICAL_MAXIMUM_3 0x27
#define PHYSICAL_MINIMUM 0x35
#define PHYSICAL_MAXIMUM 0x45
#define PHYSICAL_MAXIMUM_2 0x46
#define PHYSICAL_MAXIMUM_3 0x47
#define UNIT_EXPONENT 0x55
#define UNIT 0x65
#define UNIT_2 0x66

#define REPORT_ID       0x85
#define REPORT_COUNT    0x95
#define REPORT_COUNT_2	0x96
#define REPORT_SIZE     0x75
#define INPUT           0x81
#define FEATURE         0xb1

#define BEGIN_COLLECTION 0xa1
#define END_COLLECTION   0xc0

// Force-touch -> synthetic right-click
#define FORCE_TOUCH_PRESSURE_THRESHOLD 240

// Split a 16-bit HID item value into its little-endian byte pair, for use
// with the *_2 (2-byte) item opcodes above (e.g. LOGICAL_MAXIMUM_2,
// PHYSICAL_MAXIMUM_2). Lets per-model descriptor tables be written as plain
// decimal constants instead of hand-split hex byte pairs, which is where
// the old single-shared-descriptor bug hid: every finger collection was
// hand-written with MacBookPro16,1's literal bytes baked in, so nothing
// caught it silently applying to every other model.
#define HID_U16_LO(v) ((UCHAR)((v) & 0xFF))
#define HID_U16_HI(v) ((UCHAR)(((v) >> 8) & 0xFF))