// Decode raw USB packets into normalized touch frames.

#include "Driver.h"
#include "Input.h"

static inline USHORT
AmtInputClampCoord(_In_ INT raw, _In_ INT minVal, _In_ INT maxVal)
{
    INT shifted = raw - minVal;
    if (shifted < 0)               shifted = 0;
    if (shifted > maxVal - minVal) shifted = maxVal - minVal;
    return (USHORT)shifted;
}

VOID
AmtInputParseFrame(
    _In_  const UCHAR*                 FrameBase,
    _In_  size_t                       FingerSize,
    _In_  size_t                       RawContactCount,
    _In_  const struct BCM5974_CONFIG* DevInfo,
    _In_  LONGLONG                     TimestampQpc,
    _Out_ PRAW_FRAME                   OutFrame
)
{
    RtlZeroMemory(OutFrame, sizeof(RAW_FRAME));
    OutFrame->TimestampQpc = TimestampQpc;

    if (RawContactCount > PTP_MAX_CONTACT_POINTS)
        RawContactCount = PTP_MAX_CONTACT_POINTS;

    UCHAR emitted = 0;

    // MICRO-OPT: loop-invariant - DevInfo doesn't change per contact,
    // so this subtraction is the same value on every iteration. Hoisted
    // out instead of relying on the compiler to prove no-aliasing through
    // the DevInfo pointer.
    INT yRange = DevInfo->y.max - DevInfo->y.min;

    for (size_t i = 0; i < RawContactCount; i++) {
        const struct TRACKPAD_FINGER* f =
            (const struct TRACKPAD_FINGER*)(FrameBase + i * FingerSize);

        INT major = AmtRawToSignedInt(f->touch_major);
        INT minor = AmtRawToSignedInt(f->touch_minor);
        INT pressure = AmtRawToSignedInt(f->pressure);
        if (pressure < 0) pressure = 0; // Clamp negative pressure.

        // Skip empty contacts.
        if (major <= 0 && minor <= 0)
            continue;

        INT nx = (INT)AmtInputClampCoord(
            AmtRawToSignedInt(f->abs_x), DevInfo->x.min, DevInfo->x.max);

        INT nyRaw  = DevInfo->y.max - AmtRawToSignedInt(f->abs_y);
        INT ny     = (nyRaw < 0) ? 0 : (nyRaw > yRange ? yRange : nyRaw);

        PRAW_CONTACT rc = &OutFrame->Contacts[emitted];
        rc->SlotIndex = (USHORT)i;
        rc->X         = (USHORT)nx;
        rc->Y         = (USHORT)ny;
        rc->Major     = (USHORT)major;
        rc->Minor     = (USHORT)minor;
        rc->Pressure  = (USHORT)pressure;
        rc->Origin    = (UCHAR)f->origin;
        emitted++;
    }

    OutFrame->ContactCount = emitted;
}