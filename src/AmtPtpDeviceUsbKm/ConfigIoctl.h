// ConfigIoctl.h - AmtPtpConfigGui <-> driver custom IOCTL surface.
//
// Implements the IOCTL_AMT_PTP_* control codes declared in Public.h:
// GET/SET palm-rejection config, GET pad geometry, RESET palm config to
// defaults, and the equivalent GET/SET/RESET trio for AMT_POINTER_CONFIG
// (Force Tap threshold + action).
// Declarations for the handler functions live in Device.h (next to the
// other AmtPtpGet*/AmtPtpSet* HID handlers they sit beside in Queue.c).
//
// IMPORTANT: this header intentionally does NOT include "Driver.h" (or
// anything that pulls in Device.h/Queue.h). Neither Driver.h nor Device.h
// has an include guard / #pragma once in this codebase - every .c file in
// the project is expected to include Driver.h exactly once. ConfigIoctl.c
// already does that before including this header, so pulling Driver.h in
// again here would double-include Device.h in the same translation unit
// and redefine every type/function it declares (enums, DEVICE_CONTEXT,
// DeviceGetContext, the AmtAbsDelta/AmtDistSq/AmtRawToSignedInt inline
// helpers, etc. - exactly the C2011/C2084 errors this fix addresses).
// This header only needs EXTERN_C_START/END and L"..." string literals,
// neither of which requires any driver header.

#pragma once

EXTERN_C_START

// Registry value names used to persist AMT_PALM_CONFIG fields, one REG_DWORD
// each, under the device's "Device Parameters" software key. Kept as plain
// DWORDs (not one binary blob) so the values are individually visible/
// editable in regedit for field debugging, matching how Elan/Synaptics OEM
// panels typically expose their tunables under Device Parameters too.
#define AMT_REG_VALUE_EDGE_TOP     L"PalmEdgePermilleTop"
#define AMT_REG_VALUE_EDGE_LEFT    L"PalmEdgePermilleLeft"
#define AMT_REG_VALUE_EDGE_RIGHT   L"PalmEdgePermilleRight"
#define AMT_REG_VALUE_EDGE_BOTTOM  L"PalmEdgePermilleBottom"
#define AMT_REG_VALUE_LARGE_MAJOR  L"PalmLargeMajor"
#define AMT_REG_VALUE_LARGE_RATIO  L"PalmLargeRatio"
#define AMT_REG_VALUE_SCORE_THRESH L"PalmScoreThresh"
#define AMT_REG_VALUE_MIN_MAJOR    L"PalmMinMajor"
#define AMT_REG_VALUE_MIN_MINOR    L"PalmMinMinor"

// Registry value names for AMT_POINTER_CONFIG, same "one REG_DWORD each"
// convention as the palm values above.
#define AMT_REG_VALUE_FORCETAP_THRESHOLD L"PointerForceTapThreshold"
#define AMT_REG_VALUE_FORCETAP_ACTION    L"PointerForceTapAction"

EXTERN_C_END