/*
 * start.S — Reset entry for the Flashy read kernel.
 *
 * Called by the stock bootloader via $36 downloadAndExecute.
 * On entry: we own the CPU, interrupts already off, caches as-is.
 * Job: zero BSS, set up stack, jump to kernel_main().
 *
 * Target: GM E92 ECM — Freescale MPC5674F (PowerPC e200z7, Book E).
 * MCU confirmed via a commercial tool's shipped XML config (CPU=MPC5674F).
 */

    .section .text.start, "ax"
    .globl  _start
    .type   _start, @function

_start:
    /* Inherit the bootloader's r1 (stack pointer). stock tool's reference
     * kernel does the same — overwriting r1 with our own top-of-SRAM
     * value caused the first C-function prologue `stwu r1, -N(r1)` to
     * fault on MPC5674F (the top 16 bytes of the nominal 256K SRAM range
     * appear to be either unmapped or reserved by the bootloader), which
     * the ROM exception handler recovers from silently — looked like
     * "kernel never executed" on the bus.
     *
     * Do NOT touch r1, r2, r13: they may hold live state from the caller. */

    /* Zero BSS (linker places this empty for our kernel, but keep the
     * loop so adding .bss later doesn't silently break us). */
    lis     %r3, _bss_start@h
    ori     %r3, %r3, _bss_start@l
    lis     %r4, _bss_end@h
    ori     %r4, %r4, _bss_end@l
    li      %r0, 0
1:  cmpw    %r3, %r4
    bge     2f
    stw     %r0, 0(%r3)
    addi    %r3, %r3, 4
    b       1b
2:
    /* Jump to C. kernel_main() never returns; if it does, halt. */
    bl      kernel_main
halt:
    b       halt
    .size   _start, . - _start
