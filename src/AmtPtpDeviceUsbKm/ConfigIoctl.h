// ConfigIoctl.h - AmtPtpConfigGui <-> driver custom IOCTL surface.
//
// Implements the four IOCTL_AMT_PTP_* control codes declared in Public.h:
// GET/SET palm-rejection config, GET pad geometry, and RESET to defaults.
// Declarations for the handler functions live in Device.h (next to the
// other AmtPtpGet*/AmtPtpSet* HID handlers they sit beside in Queue.c), so
// this header only pulls in the shared driver context.

#pragma once

#include "Driver.h"

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

EXTERN_C_END