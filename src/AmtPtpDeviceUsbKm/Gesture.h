// Gesture.h - GestureEngine per-frame taint decision.
//
// Extracted from logic that previously lived inline in Interrupt.c. This
// is intentionally a SMALL extraction: the full "1 finger -> pointer, 2
// fingers -> scroll/pinch, 3+ -> system gesture" interpretation described
// in the original task spec's Phase 7 is Windows' job (the PTP HID report
// is the gesture contract with the OS) - this driver's only gesture-
// relevant responsibility is deciding which tracks get the WasInGesture
// taint. No tracking/FSM/identity logic here - matches the task spec's
// hard rule ("no tracking logic, no FSM changes, no identity
// modifications" for this layer).
//
// A prior sticky, session-scoped GESTURE_SESSION.Active field (set once
// >=2 fingers were down together and held through a 2->1 finger
// transition) was written every frame but never read by any taint
// decision - the actual taint decision (Ptpcore.c) uses only the
// per-frame AmtGestureIsMultiFingerFrame result below. The dead field
// and its update function were removed rather than wired up, since
// nothing in the codebase consumes session-level stickiness.

#pragma once

#include "PTPCore.h"

EXTERN_C_START

// TRUE if a track born or alive during THIS frame should be tainted
// (i.e. >=2 fingers are down this exact frame).
BOOLEAN
AmtGestureIsMultiFingerFrame(_In_ UCHAR AliveCount);

EXTERN_C_END
