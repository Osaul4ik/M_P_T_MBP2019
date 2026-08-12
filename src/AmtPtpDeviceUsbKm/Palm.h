// Palm classification for a single sample.

#pragma once

#include "PTPCore.h"

EXTERN_C_START

typedef enum { PALM_NONE = 0, PALM_LOCAL = 1, PALM_LARGE = 2 } PALM_CLASS;

// Classify one contact as palm or finger.
// IsBirth: TRUE when this raw contact is a fresh finger per firmware
// (Origin==0 / identity-break signal), not a continuation of a contact
// already being tracked. A birth landing inside the edge zone is a hard
// PALM_LOCAL reject regardless of shape - continuations that merely pass
// through the zone are still judged by the shape/score heuristic below.
PALM_CLASS
AmtPalmClassify(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ INT                          NormX,
    _In_ INT                          NormY,
    _In_ BOOLEAN                      IsBirth
);

EXTERN_C_END