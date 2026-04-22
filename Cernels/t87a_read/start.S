/*
 * start.S — MINIMAL TEST: infinite loop to confirm code execution.
 * If bus stays silent: code is running. If bootloader answers: not executing.
 */

    .section .text.start, "ax"
    .globl  _start
    .type   _start, @function

    /* 56-byte header — matches the layout the patched T87A boot ROM
     * expects (config word, CAN IDs, signature string, FMC base addrs).
     * The 8-byte ASCII signature at offset 0x28 is required: the
     * patched boot validator checks for this exact byte pattern before
     * permitting kernel execution. Removing it = kernel rejected. */
    .long   0x400286F0
    .long   0x00000000
    .long   0x00000000
    .long   0x00000000
    .short  0x07E2
    .short  0x0001
    .short  0x0101
    .short  0x0002
    .long   0x00000000
    .long   0x00000000
    .long   0xFFFFFFFF
    .long   0xFFFFFFFF
    .ascii  "USBJTAG\0"
    .long   0xC3F88000
    .long   0xC3F8C000

    /* offset 0x38: just spin forever */
_start:
    b       _start
    .size   _start, . - _start
