// Gesture.c - GestureEngine per-frame taint decision. See Gesture.h for
// scope notes.

#include "Driver.h"
#include "Gesture.h"

BOOLEAN
AmtGestureIsMultiFingerFrame(_In_ UCHAR AliveCount)
{
    return (AliveCount >= 2);
}
