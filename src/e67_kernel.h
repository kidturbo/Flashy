/*
 * e67_kernel.h — Kernel binary stub for E67 ECM
 *
 * When `e67_kernel_private.h` is present (gitignored), it overrides
 * these stubs with real values supplied by the user. When absent, the
 * stubs let the firmware compile — kernel upload fails at runtime
 * with a clear error message. See README / CONTRIBUTING.md for how
 * to supply your own kernel.
 */

#pragma once

#include <stdint.h>

#if __has_include("e67_kernel_private.h")
#include "e67_kernel_private.h"
#else
/* ---------------- Public stubs ---------------- */

/* E67 flash-tool read kernel (two-step) */
#define E67_KERNEL_SIZE          0
#define E67_KERNEL_LOAD_ADDR     0UL
#define E67_RD_VALUE_0           0
#define E67_RD_VALUE_1           0
#define E67_RD_VALUE_2           0
#define E67_BLOCK_SEQ            0
#define E67_SEEDKEY_ALGO         0
static const uint8_t E67_KERNEL[1] = {0};

/* E67 alternate kernel */
#define E67_EXTKERN_KERNEL_SIZE  0
#define E67_EXTKERN_LOAD_ADDR    0UL
#define E67_EXTKERN_RD_SIZE      0
static const uint8_t E67_EXTKERN_KERNEL[1] = {0};

#endif  /* __has_include */
