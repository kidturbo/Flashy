/*
 * t87a_kernel.h — Kernel binary stub for T87A TCM
 *
 * When `t87a_kernel_private.h` is present (gitignored), it overrides
 * these stubs with real values supplied by the user. When absent, the
 * stubs let the firmware compile — kernel upload fails at runtime
 * with a clear error message. The clean-room Flashy T87A kernel
 * (kernel_t87a_private.h) is the preferred path; this stub is the
 * legacy alternate-kernel slot.
 */

#pragma once

#include <stdint.h>

#if __has_include("t87a_kernel_private.h")
#include "t87a_kernel_private.h"
#else
/* ---------------- Public stubs ---------------- */

#define T87A_EXTKERN_KERNEL_SIZE 0
#define T87A_EXTKERN_LOAD_ADDR   0UL
static const uint8_t T87A_EXTKERN_KERNEL[1] = {0};

#endif  /* __has_include */
