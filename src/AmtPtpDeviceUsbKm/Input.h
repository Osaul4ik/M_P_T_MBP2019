// Decode raw USB packets into a normalized frame.

#pragma once

#include "PTPCore.h"

EXTERN_C_START

// Decode raw finger records and drop empty contacts.
VOID
AmtInputParseFrame(
    _In_  const UCHAR*                 FrameBase,
    _In_  size_t                       FingerSize,
    _In_  size_t                       RawContactCount,
    _In_  const struct BCM5974_CONFIG* DevInfo,
    _In_  LONGLONG                     TimestampQpc,
    _Out_ PRAW_FRAME                   OutFrame
);

EXTERN_C_END