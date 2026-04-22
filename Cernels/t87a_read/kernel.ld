/*
 * kernel.ld — Linker script for SRAM-resident read kernel
 *
 * Target: GM T87A TCM — ST SPC564A80 (e200z4)
 *   - Internal SRAM: 192 KB at 0x40000000
 *   - Kernel loads at 0x40010000 (matches stock-tool kernel load address)
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
    SRAM (rwx) : ORIGIN = 0x40010000, LENGTH = 128K
}

SECTIONS
{
    .text 0x40010000 : {
        KEEP(*(.text.start))     /* _start must be first */
        *(.text)
        *(.text.*)
        KEEP(*(.rodata))
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

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}
