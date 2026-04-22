# Makefile — Flashy clean-room read kernel for GM E92 ECM
#            (Freescale MPC5674F, PowerPC e200z7).
#
# Toolchain: powerpc-eabi-gcc (Book E, big-endian, no FPU).
#   Easiest source: NXP S32 Design Studio for Power Architecture
#   (free, registration required) — bundles a prebuilt toolchain.
#   Alternatively: SPC5Studio from ST, or a community powerpc-eabivle build.
#
# Override TOOLCHAIN_PREFIX from the command line if the binaries are
# not in PATH, e.g.:
#   make TOOLCHAIN_PREFIX=/c/nxp/S32DS.Power/Cross_Tools/powerpc-eabivle-4_9/bin/powerpc-eabivle-

TOOLCHAIN_DIR    ?= C:/NXP/powerpc-eabivle-4_9/bin
TOOLCHAIN_PREFIX ?= $(TOOLCHAIN_DIR)/powerpc-eabivle-
CC      := $(TOOLCHAIN_PREFIX)gcc
AS      := $(TOOLCHAIN_PREFIX)gcc
LD      := $(TOOLCHAIN_PREFIX)gcc
OBJCOPY := $(TOOLCHAIN_PREFIX)objcopy
OBJDUMP := $(TOOLCHAIN_PREFIX)objdump
SIZE    := $(TOOLCHAIN_PREFIX)size

# Target: e200z7 (MPC5674F — GM E92 ECM) in Book E mode. VLE-capable
# toolchain, but we emit classic Book E here so the reset handoff from
# the stock bootloader lands in a known MSR[IS]=0 state.
ARCHFLAGS := -mcpu=e200z7 -mbig -Wa,-mbooke -mno-vle
CFLAGS    := $(ARCHFLAGS) -std=gnu11 -Os -Wall -Wextra -ffreestanding \
             -fno-builtin -fno-common -fno-stack-protector -nostdlib \
             -msdata=none
LDFLAGS   := $(ARCHFLAGS) -nostdlib -Wl,-T,kernel.ld -Wl,-Map,kernel.map \
             -Wl,--gc-sections

OBJS := start.o main.o

all: kernel.bin

start.o: start.S
	$(AS) $(CFLAGS) -c -o $@ $<

main.o: main.c flexcan.h
	$(CC) $(CFLAGS) -c -o $@ $<

kernel.elf: $(OBJS) kernel.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	$(SIZE) $@

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary --only-section=.text --only-section=.rodata \
	           --only-section=.data $< $@
	@echo "=== $@ built ==="

kernel.lst: kernel.elf
	$(OBJDUMP) -d $< > $@

clean:
	rm -f *.o *.elf *.bin *.map *.lst

.PHONY: all clean
