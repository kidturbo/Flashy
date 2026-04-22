# Cernels — Flashy clean-room Kernels

*C-kernels. SRAM-resident. Built from public docs, not from anyone else's bytes.*

PowerPC e200 kernels for GM ECU flash operations, written from public documentation only. No third-party kernel code is referenced, included, or reused.

## Why this exists

The stock GM bootloader on MPC5xxx-family ECUs has no flash-programming primitives accessible over CAN. To read or write flash, an out-of-band kernel must be uploaded into SRAM via UDS `$36` and executed. Commercial flash tools ship proprietary kernels to do this — those are their copyright, not something we can redistribute.

These kernels are ours. We wrote them. MIT licensed. Ship freely.

## Targets

| Directory | ECU | MCU | Status |
|-----------|-----|-----|--------|
| `e92_read/` | E92 ECM | **Freescale MPC5674F** (e200z7, 4 MB flash, 256 KB SRAM) | v0.1 source, builds, upload tested on bench |
| `e67_read/` | E67 ECM | MPC5xxx-family (variant unconfirmed) | placeholder — see directory README |

> **Note on MCU identification.** E92 is now confirmed as **Freescale MPC5674F** via a commercial flash tool's shipped XML config (declares `<CPU>MPC5674F</CPU>` for E92 BAM mode). e200z7 Book E core, dual flash modules (FMC0 @ `0xC3F88000`, FMC1 @ `0xC3F8C000`), same flash controller architecture as T87A's SPC564A80. E67's exact MCU remains unconfirmed pending a board photo or JTAG IDCODE read.

## Building

Needs a `powerpc-eabi(-vle)` GCC toolchain. The free options:

1. **NXP Embedded GCC for Power Architecture v4.9.4** (standalone, portable, no installer) — what this project currently uses. On this dev machine: `C:\NXP\powerpc-eabivle-4_9\`.
2. **NXP S32 Design Studio for Power Architecture** — full IDE, [free with NXP registration](https://www.nxp.com/design/design-center/software/automotive-software-and-tools/s32-design-studio-ide:S32-DESIGN-STUDIO-IDE). Bundles the same compiler.
3. **ST SPC5Studio** — similar, ST-branded, free.

Once installed, build via VSCode: `Terminal → Run Task → Build Kernel (E92)`. Or from the shell:

```bash
cd Cernels/e92_read
make TOOLCHAIN_DIR=/c/NXP/powerpc-eabivle-4_9/bin
```

Produces `kernel.bin` (raw binary suitable for upload) and `kernel.elf` (for debugging / disassembly).

## Upload + run

Once built, convert the kernel.bin into a private header and rebuild the Feather firmware:

```bash
python tools/bin2header.py Cernels/e92_read/kernel.bin \
    src/kernel_e92.h KERNEL_E92 0x40001000
pio run -t upload
```

> The output file `src/kernel_e92.h` is tracked in git (it's our own MIT-licensed work, not a proprietary kernel). Regenerating it overwrites the existing tracked copy — review the diff before committing.

Then on the bench module:

```
INIT 500000
ALGO e92
CERNEL            # session entry + $34/$36 80 upload + FLSHY ACK wait
```

The kernel announces itself with `ACK "FLSHY"` on `0x7E8` when running. Issue raw `CMD_READ` (0xA0) frames on `0x7E0` to stream memory back (host-side driver still TBD).

## Wire protocol (v0.1)

Request frame (tester → ECU, CAN ID `0x7E0`, DLC 8):

| Byte | Field |
|------|-------|
| 0 | Command |
| 1-4 | Address (big-endian) |
| 5-6 | Length (big-endian, bytes) |
| 7 | reserved |

Commands:

| Code | Name | Behaviour |
|------|------|-----------|
| `0x01` | PING | Responds with `41 "FLSHY" 01 00` |
| `0xA0` | READ | Streams `length` bytes back, 8 per frame |

Response frame (ECU → tester, CAN ID `0x7E8`, DLC 1-8): raw bytes per command.

## Legal posture

- **Source**: our own, written from public Freescale / ST reference manuals + FlexCAN register definitions. No third-party kernel disassembly was used as a source.
- **Embedded copyright**: `(c)2026 FLASHY E67__v0.1C - Rolling Smoke Kernel by Claude + kidturbo. MIT.` (the `E67` tag in the signature predates the rename — will become `E92` on next kernel rebuild)
- **License**: MIT. Same as the rest of Flashy. Copy, modify, sell, embed — just keep the notice.
