/*
 * kernel.ld — Linker script for SRAM-resident read kernel
 *
 * Target: GM E92 ECM — Freescale MPC5674F (e200z7)
 *   - Internal SRAM: 256 KB at 0x40000000
 *   - Kernel loads at 0x40001000 (leaves low 4 KB for stack/scratch)
 *   - Flash: 4 MB (0x00000000-0x003FFFFF), dual modules
 *     FMC0 @ 0xC3F88000, FMC1 @ 0xC3F8C000
 *
 * Output: raw binary extracted from ELF for upload via UDS $36.
 */

OUTPUT_FORMAT("elf32-powerpc", "elf32-powerpc", "elf32-powerpc")
OUTPUT_ARCH(powerpc)
ENTRY(_start)

MEMORY
{
    SRAM (rwx) : ORIGIN = 0x40001000, LENGTH = 252K
}

SECTIONS
{
    .text 0x40001000 : {
        KEEP(*(.text.start))     /* _start must be first */
        *(.text)
        *(.text.*)
        KEEP(*(.rodata))         /* signature string must survive --gc-sections */
        KEEP(*(.rodata.*))
        . = ALIGN(4);
    } > SRAM

    .data : {
        *(.data)
        *(.data.*)
        . = ALIGN(4);
    } > SRAM

    .bss (NOLOAD) : {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > SRAM

    /* Stack at the top of the kernel region. */
    . = ALIGN(16);
    _stack_top = ORIGIN(SRAM) + LENGTH(SRAM) - 16;

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
