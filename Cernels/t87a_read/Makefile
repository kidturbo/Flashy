# Makefile — Flashy clean-room read kernel for GM T87A TCM
#            (ST SPC564A80, PowerPC e200z4, VLE).
#
# Toolchain: powerpc-eabivle-gcc (VLE, big-endian, no FPU).

TOOLCHAIN_DIR    ?= C:/NXP/powerpc-eabivle-4_9/bin
TOOLCHAIN_PREFIX ?= $(TOOLCHAIN_DIR)/powerpc-eabivle-
CC      := $(TOOLCHAIN_PREFIX)gcc
AS      := $(TOOLCHAIN_PREFIX)gcc
LD      := $(TOOLCHAIN_PREFIX)ld
OBJCOPY := $(TOOLCHAIN_PREFIX)objcopy
OBJDUMP := $(TOOLCHAIN_PREFIX)objdump
SIZE    := $(TOOLCHAIN_PREFIX)size

# Target: e200z4 (SPC564A80 — GM T87A TCM) in VLE mode.
# Try Book E first — bootloader TLB may have VLE=0 for SRAM region.
# e200z4 supports VLE but bootloader controls the mode via TLB.
ARCHFLAGS := -mcpu=e200z4 -mbig -Wa,-mbooke -mno-vle
CFLAGS    := $(ARCHFLAGS) -std=gnu11 -Os -Wall -Wextra -ffreestanding \
             -fno-builtin -fno-common -fno-stack-protector -nostdlib \
             -msdata=none
LDFLAGS   := -T kernel.ld -Map kernel.map --gc-sections

OBJS := start.o main.o

all: kernel.bin

start.o: start.S
	$(AS) $(CFLAGS) -c -o $@ $<

main.o: main.c flexcan.h
	$(CC) $(CFLAGS) -c -o $@ $<

kernel.elf: $(OBJS) kernel.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary --only-section=.text --only-section=.rodata \
	           --only-section=.data $< $@
	@echo "=== $@ built ==="

kernel.lst: kernel.elf
	$(OBJDUMP) -d $< > $@

clean:
	rm -f *.o *.elf *.bin *.map *.lst

.PHONY: all clean
