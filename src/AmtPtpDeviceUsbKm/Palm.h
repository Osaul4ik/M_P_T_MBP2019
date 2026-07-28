// Palm.h - Palm classification.
//
// Extracted verbatim (scoring logic unchanged) from the old
// AmtClassifyPalm in Match.c. Per-sample classification only; this
// file makes no session-level decisions (PalmDetected latch, whole-pad
// blank persistence) - that orchestration stays where the rest of the
// per-frame session state lives, since it is symmetrical with
// GestureThisFrame (see Match.c/Interrupt.c/PTPCore.c). This file answers
// exactly one question per finger: "does this single sample look like
// a palm", nothing more - consistent with Phase 6 of the task spec
// ("ONLY: suppress reporting; DO NOT affect identity/FSM/tracking").

#pragma once

#include "PTPCore.h"

EXTERN_C_START

typedef enum { PALM_NONE = 0, PALM_LOCAL = 1, PALM_LARGE = 2 } PALM_CLASS;

// Per-sample palm classification. Pure function of the raw geometry +
// normalized position; no state read or written.
PALM_CLASS
AmtPalmClassify(
    _In_ USHORT                       Major,
    _In_ USHORT                       Minor,
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ INT                          NormX,
    _In_ INT                          NormY
);

// Birth-only suppression zone at the physical bottom edge (near the
// click hinge / palm-rest area). Distinct from AmtPalmClassify: this
// says nothing about shape and must ONLY be consulted for a contact
// that is being born this frame (RAW_CONTACT.Origin == 0 / identity
// break). A contact already active and dragging through this same
// region must NOT be suppressed - the caller is responsible for only
// calling this on birth frames.
BOOLEAN
AmtPalmInBottomBirthZone(
    _In_ const struct BCM5974_CONFIG* DevInfo,
    _In_ INT                          NormY
);

EXTERN_C_END
