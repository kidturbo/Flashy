/*
 * gm_kernels.h — Kernel binary stub
 *
 * Flashy ships only its own clean-room Kernels (in the Cernels/ tree).
 * The defines and arrays below describe the loader interface used by
 * cmd_kernel() in main.cpp. When a `gm_kernels_private.h` is present
 * (gitignored), it overrides these stubs with real values supplied by
 * the user. When absent, the stubs let the firmware compile and run
 * — but kernel uploads fail at runtime with a clear error message.
 *
 * To supply your own kernels, see the project README and CONTRIBUTING.md
 * for the workflow (capture, extract, drop into src/gm_kernels_private.h).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if __has_include("gm_kernels_private.h")
#include "gm_kernels_private.h"
#else
/* ---- Module type enum (private header defines it when present) ---- */
typedef enum {
    MODULE_T87  = 0,
    MODULE_E38  = 1,
    MODULE_T42  = 2,
    MODULE_E92  = 3,
    MODULE_E38N = 4,  /* E38 with alternate read+write kernel */
    MODULE_E67  = 5,  /* E67 ECM — same flash as E38, different seed-key */
    MODULE_E40  = 6,  /* E40 ECM — placeholder, diag-only until kernel/algo available */
    MODULE_T93  = 7,  /* T93 TCM — same hardware as T87A (SPC564A80), different algo */
    MODULE_NONE = 255 /* boot default — user has not selected a module yet */
} gm_module_t;

/* ---------------- Public stubs ---------------- */
/* All sizes are 0 so cmd_kernel() can detect "no kernel embedded" and
 * return a clear error message. Memory map constants are also 0 to
 * keep dependent compile-time arithmetic safe. */

/* T87 read kernel */
#define T87_KERNEL_SIZE          0
#define T87_KERNEL_LOAD_ADDR     0UL
#define T87_RD_VALUE_0           0
#define T87_RD_VALUE_1           0
#define T87_RD_VALUE_2           0
#define T87_BLOCK_SEQ            0
#define T87_SEEDKEY_ALGO         0
static const uint8_t T87_KERNEL[1] = {0};

/* T87 cal-write kernel */
#define T87W_CAL_KERNEL_SIZE     0
static const uint8_t T87W_CAL_KERNEL[1] = {0};

/* T87 full-write kernel */
#define T87W_FULL_KERNEL_SIZE    0
static const uint8_t T87W_FULL_KERNEL[1] = {0};

/* E38 read kernel */
#define E38_KERNEL_SIZE          0
#define E38_KERNEL_LOAD_ADDR     0UL
#define E38_RD_VALUE_0           0
#define E38_RD_VALUE_1           0
#define E38_RD_VALUE_2           0
#define E38_BLOCK_SEQ            0
#define E38_SEEDKEY_ALGO         0
#define E38_FLASH_SIZE           0UL
#define E38_BLOCK_COUNT          0
#define E38_ADDR_ECHO_SKIP       0
#define E38_CAL_START            0UL
#define E38_CAL_SIZE             0UL
#define E38_CAL_BLOCK_START      0
#define E38_CAL_BLOCK_COUNT      0
static const uint8_t E38_KERNEL[1] = {0};

/* E38 write kernel (two-step) */
#define E38W_KERNEL_SIZE         0
#define E38W_KERNEL_LOAD_ADDR    0UL
#define E38W_RD_VALUE_0          0
#define E38W_RD_VALUE_1          0
#define E38W_RD_VALUE_2          0
#define E38W_BLOCK_SEQ           0
static const uint8_t E38W_KERNEL[1] = {0};

/* E38 alternate kernel */
#define E38N_KERNEL_SIZE         0
#define E38N_KERNEL_LOAD_ADDR    0UL
#define E38N_RD_VALUE_0          0
#define E38N_RD_VALUE_1          0
#define E38N_RD_VALUE_2          0
#define E38N_BLOCK_SEQ           0
static const uint8_t E38N_KERNEL[1] = {0};

/* T87 memory map facts (kept at 0 so block math doesn't try to read flash) */
#define T87_FLASH_SIZE           0UL
#define T87_BLOCK_COUNT          0
#define T87_ADDR_ECHO_SKIP       0
#define T87_CAL_START            0UL
#define T87_CAL_SIZE             0UL
#define T87_CAL_BLOCK_START      0
#define T87_CAL_BLOCK_COUNT      0
#define T87_WRITE_CAL_START      0UL
#define T87_WRITE_CAL_SIZE       0UL
#define T87_WRITE_CAL_BLOCKS     0
#define T87_WRITE_FULL_START     0UL
#define T87_WRITE_FULL_SIZE      0UL
#define T87_WRITE_FULL_BLOCKS    0

/* Seed-key bytecode arrays (13 bytes each).
 * All zeros means the bytecode interpreter exits early with key == seed,
 * which the ECU rejects as an invalid key — clear failure mode. */
static const uint8_t SEEDKEY_T87[13] = {0};
static const uint8_t SEEDKEY_E38[13] = {0};
static const uint8_t SEEDKEY_T42[13] = {0};
static const uint8_t SEEDKEY_E67[13] = {0};
static const uint8_t SEEDKEY_E92[13] = {0};

#endif  /* __has_include */

#ifdef __cplusplus
}  /* extern "C" */
#endif
