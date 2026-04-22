# Flashy — J2534 Pass-Thru

![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)
![Platform: Feather M4 CAN](https://img.shields.io/badge/Platform-Feather_M4_CAN-blue.svg)
![Build: PlatformIO](https://img.shields.io/badge/Build-PlatformIO-orange.svg)

A J2534-style Pass-Thru tool for **diagnostics, reversing, and programming automotive ECUs**, built on the **Adafruit Feather M4 CAN Express (ATSAME51J19A)**. The device bridges a PC (USB serial) to the vehicle CAN bus (OBD-II or bench harness) to read, write, and interact with firmware on ECMs, TCMs, BCMs, and other CAN nodes. Currently tested with the modules listed in the [ECU Reference Guide](https://kidturbo.github.io/Flashy/ecu-reference.html).

<p align="center">
  <img src="docs/images/hardware/Flashy1.jpg" alt="Flashy on Feather M4 CAN Express" width="500">
</p>

**[Download Latest Release](https://github.com/kidturbo/Flashy/releases/latest)** · **[Wiki](https://github.com/kidturbo/Flashy/wiki)** · **[Report a Bug](https://github.com/kidturbo/Flashy/issues/new?template=bug_report.md)** · **[Request a Feature](https://github.com/kidturbo/Flashy/issues/new?template=feature_request.md)**

## Quick Start

1. **Get the hardware** — [Feather M4 CAN Express](https://www.adafruit.com/product/4759) + [AdaLogger FeatherWing](https://www.adafruit.com/product/2922) (recommended)
2. **Build & flash** — `pio run -t upload` (see [Build](#build) below)
3. **Get a kernel** — extract from a CAN capture or build the [clean-room Kernels](Cernels/README.md)
4. **Connect to vehicle** — plug into OBD-II, open serial terminal at 115200 baud
5. **Read your first ECU:**
   - Type `MENU` — interactive guided flow: pick your module, then read / write / diag
   - CAN auto-initializes at boot (AUTOINIT) and auto-detects baud, so there's nothing to configure first
   - Type `HELP` any time for the full command list, or `STATUS` to see connection state
6. **Or skip building** — download the [portable Windows tools](https://github.com/kidturbo/Flashy/releases/latest) (no Python needed)

## Hardware

The reference build uses two stacked Adafruit Feather boards — a Feather M4 CAN Express (CAN transceiver + MCU) and an AdaLogger FeatherWing (microSD + RTC) for on-device logging and captures.

| Part | Purpose | Link |
|------|---------|------|
| [Adafruit Feather M4 CAN Express](https://www.adafruit.com/product/4759) | ATSAME51J19A MCU with built-in CAN transceiver (`MCP2562FD`) and 5V boost for the bus | [Adafruit #4759](https://www.adafruit.com/product/4759) |
| [Adafruit AdaLogger FeatherWing](https://www.adafruit.com/product/2922) | microSD + RTC stack for Feather — used to stream reads/writes directly to SD card | [Adafruit #2922](https://www.adafruit.com/product/2922) |

**Pinout references:**

- [Feather M4 CAN Express pinout](https://learn.adafruit.com/adafruit-feather-m4-can-express/pinouts) — official Adafruit pin map for the main board
- [AdaLogger FeatherWing pinout](https://learn.adafruit.com/adafruit-adalogger-featherwing/pinouts) — SD card + RTC pin map
- [AdaLogger FeatherWing assembly](https://learn.adafruit.com/adafruit-adalogger-featherwing/assembly) — header-soldering options (see note below)

> **Terminal block mounting — read before you solder:** The 3-pin CAN-bus screw terminal that ships with the Feather M4 CAN is taller than the standard pin headers on the AdaLogger, and its screws face *upward*. If you mount the terminal block on top of the Feather in the "normal" position with the AdaLogger stacked above, you can't get a screwdriver onto the terminal screws. Taller headers don't fix this — even with clearance, the wing still blocks screwdriver access.
>
> Mounting the AdaLogger *below* the Feather isn't a good workaround either — it covers the wing's coin-cell RTC battery holder, making future battery swaps a pain.
>
> **Recommended:** mount the terminal block on the **bottom side** of the Feather. Screwdriver access comes from the top (unobstructed), and the AdaLogger sits normally on top with the RTC battery reachable. This is what the reference build uses — see the [Build Photos](#build-photos) above.
>
> **Alternatives:**
>
> - Solder CAN-H, CAN-L, and GND wires directly to the Feather's pads (skip the terminal block entirely)
> - Use a flatter/shorter 3-pin terminal block with side-entry screws

### Build Photos

<p align="center">
  <img src="docs/images/hardware/Flashy1.jpg" alt="Feather M4 CAN Express" width="320">
  <img src="docs/images/hardware/Flashy2.jpg" alt="AdaLogger FeatherWing" width="320">
  <img src="docs/images/hardware/Flashy3.jpg" alt="Stacked assembly powered on" width="320">
</p>

<p align="center">
  <em>Left: Feather M4 CAN Express (main board). Middle: AdaLogger FeatherWing with RTC + SD card. Right: Stacked assembly powered up and running.</em>
</p>

See the [Hardware Assembly wiki page](https://github.com/kidturbo/Flashy/wiki/Hardware-Assembly) for step-by-step build instructions, including how to break the 120Ω CAN terminator for vehicle use.

### Cut the 120Ω CAN Terminator — **before stacking the wing**

<p align="center">
  <img src="docs/images/hardware/Feather-Cut-120ohm-Terminating-Resistor.jpg" alt="Where to cut the Trm jumper on the Feather M4 CAN" width="520">
</p>

The Feather ships with its onboard 120Ω CAN terminator bridged. Since most vehicle CAN buses are already terminated at each end, this third terminator will load the bus and cause errors. Cut the `Trm` jumper (shown above, next to the CAN-H screw terminal) **before you stack the AdaLogger** — once stacked, the jumper is no longer reachable.

### Other Arduino-compatible hardware

The firmware is written against the standard Arduino framework (via PlatformIO) and the Adafruit CAN library (`CANSAME5x`), so it should port to any SAME5x-family board with a CAN peripheral. SD card support is optional — remove the SdFat dependency and SD-aware commands if your board doesn't have a card. Pin definitions and the CAN transceiver enable/standby lines may need tweaking for non-Feather boards; see `src/main.cpp` (`PIN_CAN_STANDBY`, `PIN_CAN_BOOSTEN`) and `platformio.ini` for the starting point.

## Supported Modules

All listed modules respond to VIN scan, VIN read/write, OSID read, and CAN bus capture **out of the box** in the public release. Flash read/write needs either the clean-room kernel (E92 today) or a user-supplied kernel header — see the "With your own kernel" column. Live status is tracked in the [ECU Reference Guide](https://kidturbo.github.io/Flashy/ecu-reference.html) and the [wiki](https://github.com/kidturbo/Flashy/wiki).

| Module | Type | CAN IDs | Flash | Public build (v1.4.0) | With your own kernel |
|--------|------|---------|-------|-----------------------|----------------------|
| **E92** | ECM | 0x7E0 / 0x7E8 | 4 MB | ✓ **Full Read** (clean-room kernel) | — |
| E38 | ECM | 0x7E0 / 0x7E8 | 2 MB | VIN + diagnostics | Read + Write (algo 402) |
| E67 | ECM | 0x7E0 / 0x7E8 | 2 MB | VIN + diagnostics | Read (algo 393) |
| T87 | TCM | 0x7E2 / 0x7EA | 4 MB | VIN + diagnostics | Read + Write (algo 569) |
| T87A | TCM (10L/8L) | 0x7E2 / 0x7EA | 4 MB | VIN + diagnostics | BAM + HS-CAN read/write (5-byte algo) |
| T93 | TCM | 0x7E2 / 0x7EA | 4 MB | VIN + diagnostics | *In progress* |
| T42 | TCM | 0x7E2 / 0x7EA | 1 MB | VIN + diagnostics | *Planned* |
| E40 | ECM | 0x7E0 / 0x7E8 | 1 MB | VIN + diagnostics | *Planned* |

> Want flash read/write on a non-E92 module today? See [CONTRIBUTING.md](.github/CONTRIBUTING.md) for the workflow to extract a kernel from a CAN capture of a tool you own and rebuild firmware locally. New clean-room kernels will land in future releases — track the [changelog](https://kidturbo.github.io/Flashy/changelog.html).

### Specs

- **MCU**: ATSAME51J19A — 120 MHz Cortex-M4F, 512 KB flash, 192 KB RAM
- **CAN**: Built-in SAME51 CAN peripheral + on-board transceiver with 5V boost
- **Library**: Adafruit CAN (CANSAME5x)
- **Board target**: `adafruit_feather_m4_can`

> **Strongly recommended: use the SD Wing.**
>
> Large transfers (4 MB flash reads, full-flash writes, large `.bin` uploads) over the USB serial link are slow and error-prone. Observed issues without the SD Wing include:
> - **SDWRITE buffer truncation** &mdash; hex-encoded uploads over USB serial have historically truncated at 2 MB on certain host stacks. A 4 MB flash dump copied to the Feather over USB can silently lose half its data.
> - **Throughput limits** &mdash; USB serial at 115,200 baud caps host→device transfer at ~11 KB/s even under ideal conditions. Reading an ECU dump and streaming it back to the PC roughly doubles operation time compared to going direct to SD.
> - **Host-side reliability** &mdash; long USB sessions are vulnerable to COM-port timeouts, driver hiccups, or the user accidentally closing the terminal mid-write, any of which can brick an ECU mid-flash.
>
> The AdaLogger FeatherWing sidesteps all of the above: reads stream directly to SD, writes read their source from SD, and the PC only needs to be connected to kick off the operation. **For any read or write over ~512 KB, use the SD path.** Copy the source `.bin` onto the card by hand (pull the card, drop the file, reinsert) or use `tools/sd_upload.py` for small files.

## Architecture

```
┌─────────────────────────────┐
│  Serial Command Interface   │  ← PC sends INIT / READ / WRITE / AUTH / etc.
│  (src/main.cpp)             │
├─────────────────────────────┤
│  UDS Layer (src/uds.c/.h)   │  ← ISO 14229 services
├─────────────────────────────┤
│  ISO-TP (lib/isotp/)        │  ← ISO 15765-2 segmentation (lishen2/isotp-c, MIT)
├─────────────────────────────┤
│  CAN Driver (src/can_driver)│  ← CANSAME5x wrapper (extern "C")
├─────────────────────────────┤
│  Hardware (SAME51 CAN)      │
└─────────────────────────────┘
```

## Build

PlatformIO (recommended):

```bash
pio run              # compile
pio run -t upload    # flash to Feather
```

Config: `platformio.ini`. Dependencies auto-managed (Adafruit CAN + vendored isotp-c).

## Directory Layout

```
src/                   Firmware sources
  main.cpp             Serial command parser, top-level state machine
  can_driver.*         CANSAME5x wrapper
  uds.c/.h             UDS service layer (pure C)
  seed_key.c/.h        GM 2-byte seed-key bytecode interpreter
  gm5byte_key.c/.h     GM 5-byte AES-128 / SHA-256 key derivation
  gm_kernels.h         Kernel stub (expects gm_kernels_private.h)
  e67_kernel.h         Kernel stub (expects e67_kernel_private.h)
  t87a_kernel.h        Kernel stub (expects t87a_kernel_private.h)
  Pass-Thru-Protocol.h Project-wide config + defines
lib/isotp/             Vendored lishen2/isotp-c (MIT)
tools/                 Python host-side tools
  capture_read.py      Automated FULLREAD → .bin
  capture_write.py     Flash write driver
  vin_scan.py          Multi-module VIN scanner
  vin_update.py        Single-module VIN updater
  detect_port.py       Auto-detect Feather USB COM port
  seed_key_algo.py     Python seed→key (all 1280 GM algorithms)
  t87a_patch.py        T87A 5-patch dual-unlock tool + checksum recalc
  t87_calwrite.py      T87 calibration-only write
  t87_fullwrite.py     T87 full-flash write
  sd_upload.py         Upload files to Feather SD card over serial
  gm5byte/             Python reference implementation of 5-byte key
  build_exe.py         PyInstaller build → portable .exe distribution
  capture_bus.py       CAN bus → SavvyCAN-format CSV (drives CAPTURE cmd)
  extract_kernel.py    Extract kernel bytes from an SavvyCAN-style CSV
  extract_kernel_from_csv.py  Generic CSV kernel extractor
  extract_usbjtag_kernel.py   USBJTAG-style CAN capture extractor
Cernels/               Clean-room PowerPC Kernel source (MIT)
  e92_read/            E92 ECM Book E read kernel (MPC5xxx-family, MCU TBD)
  e67_read/            E67 ECM placeholder (MCU TBD)
```

## Serial Commands

```
INIT [baud]           Init CAN bus (default 500000)
SETID <tx> <rx>       Set tester/ECU CAN IDs (hex)
ALGO <e38|e67|t87|...>  Select module + seed-key algorithm
DIAG [session]        DiagnosticSessionControl
AUTH [key_hex]        SecurityAccess (seed → auto-compute key)
KERNEL                Upload kernel to ECU
FULLREAD              Automated full flash read
CALREAD               Calibration-only read (E38)
READ <addr> <blocks>  Manual block read
WRITE <addr> <blocks> Write blocks from PC
CALWRITE              Automated cal write + verify
FULLWRITE             Automated full flash write + verify
BAMREAD               T87A: BAM boot-mode full read
BAMWRITE [file]       T87A: BAM boot-mode write + verify
VIN                   Read VIN
VINWRITE <vin>        Write 17-character VIN
SCAN                  Scan bus for all modules + VINs
OSID                  Read OS/Calibration ID
ERASE <addr> <size>   Erase flash region
RESET [type]          ECU Reset
RAW <hex>             Send raw UDS request
CANSEND <id> <hex>    Send raw CAN frame
STATUS                Show connection state
MENU                  Interactive module menu
HELP                  List commands
```

## Kernel Binaries

The firmware needs a small PowerPC "kernel" resident in the ECU's SRAM to perform flash reads and writes. GM's bootloader does not provide the primitives directly — a short loader is uploaded over ISO-TP, executed in RAM, and then drives the flash controller.

**This public repository does not ship kernel binaries.** Kernels used by commercial flash tools (EFILive, HPTuners, USBJTAG-based tools, etc.) are the copyrighted property of their authors, and redistributing those compiled binaries — even embedded as hex arrays — is not something this project takes a position on. The source headers `src/gm_kernels.h`, `src/e67_kernel.h`, and `src/t87a_kernel.h` are **stubs**: they use `__has_include` to fall back to a `*_private.h` file that you must supply.

To build the firmware you have two options:

### Option 1 — Extract from a CAN capture of a tool you own

If you have a licensed flash tool (EFILive, HPTuners, NTLink/USBJTAG, U-Link, etc.) and can capture its CAN traffic during a read or write session (e.g., with [SavvyCAN](https://savvycan.com/)), the extractors can pull the kernel bytes out of the capture and emit a header you can drop into `src/`:

```bash
python tools/extract_kernel.py             path/to/capture.csv   > src/gm_kernels_private.h
python tools/extract_kernel_from_csv.py    path/to/capture.csv   # generic / matcher
python tools/extract_usbjtag_kernel.py     path/to/capture.csv   # USBJTAG-style raw-CAN uploads
```

The extractors look for the signature byte sequences (`(c)YYYY <tool> <ecu>__vX.YR`) that are present in the kernel upload frames, walk back to the `$36 80` transfer block, and concatenate the payload to produce a clean binary. This is ordinary format-parsing — it processes data you already lawfully possess and does not itself contain anyone else's code.

### Option 2 — Build the clean-room Kernels

This repo ships its own PowerPC kernel source tree at [`Cernels/`](Cernels/) — MIT-licensed, written from public Freescale / ST reference manuals and FlexCAN register specifications, with no third-party kernel code referenced. Build it with the free NXP `powerpc-eabivle-gcc` toolchain, drop the resulting bytes into `src/<ecu>_kernel_private.h`, and you're running 100% your own firmware end-to-end.

See [`Cernels/README.md`](Cernels/README.md) for build instructions, toolchain setup, and the current per-ECU status.

If a `*_private.h` is missing at build time, PlatformIO will stop with an `#error` directing you back here.

## T87A Dual Unlock

The `tools/t87a_patch.py` utility applies the 5-patch recipe (USBJTAG signature bypass + HPT unlock) and recalculates the Boot Block CRC16 + Wordsum checksums. See the [T87A-Unlock-Recipe](https://kidturbo.github.io/Flashy/T87A-Unlock-Recipe.html) for the full recipe, flash memory map, and verified OS versions.

```bash
python tools/t87a_patch.py input.bin output.bin       # Apply patches
python tools/t87a_patch.py --verify file.bin          # Verify checksums
python tools/t87a_patch.py --batch directory/         # Batch patch
```

## Documentation

- [ECU Reference Guide](https://kidturbo.github.io/Flashy/ecu-reference.html) — MCU specs, memory maps, and verification status for all supported modules
- [J2534-CANbus-Reference](https://kidturbo.github.io/Flashy/J2534-CANbus-Reference.html) — full protocol reference
- [Read-Write-Instructions](https://kidturbo.github.io/Flashy/Read-Write-Instructions.html) — usage guide and workflow
- [T87A-Unlock-Recipe](https://kidturbo.github.io/Flashy/T87A-Unlock-Recipe.html) — T87A 5-patch dual-unlock recipe
- [Changelog](https://kidturbo.github.io/Flashy/changelog.html) — version history

## Downloads

Pre-built Windows tools (no Python required):

**[Latest Release](https://github.com/kidturbo/Flashy/releases/latest)** — download `Flashy-Tool.zip`

Each release includes portable `.exe` tools for reading, writing, VIN scanning, and more. See the [release process](docs/RELEASING.md) for versioning details and the [Downloads wiki page](https://github.com/kidturbo/Flashy/wiki/Downloads) for a full file inventory.

## Contributing

See [CONTRIBUTING.md](.github/CONTRIBUTING.md) for build instructions, testing phases, and pull request guidelines.

Found a bug? **[Open an issue](https://github.com/kidturbo/Flashy/issues/new?template=bug_report.md)** — the template will guide you through what info to include.

## License

Source code in this repository is released under the [MIT License](LICENSE). See individual files for any additional notices. The vendored `lib/isotp/` is from [lishen2/isotp-c](https://github.com/lishen2/isotp-c) (MIT).

### Third-party material

- **GM part numbers, calibration IDs, seed-key algorithm numbers, and OS identifiers** referenced in documentation and code are factual descriptors of hardware this project interoperates with. They remain the property of General Motors. No claim of affiliation with or endorsement by GM is made or implied.
- **Commercial flash-tool kernels** (EFILive, HPTuners, NTLink/USBJTAG, U-Link, etc.) are the copyrighted property of their respective authors. **This repository does not ship those binaries in any form.** Kernel header stubs in `src/` expect the user to supply a `*_private.h` file via the extraction workflow (see [Kernel Binaries](#kernel-binaries)) or a self-written replacement.
- **The extraction tools** (`tools/extract_kernel*.py`) perform format parsing on CAN-bus capture files supplied by the user. They do not themselves contain, embed, or redistribute any third-party kernel code. They are analogous to file-format parsers or disassemblers: a user processing their own capture of their own licensed tool is performing a lawful operation on data they already possess.
- **Arduino/PlatformIO framework code** pulled in during the build is subject to its own licenses (primarily LGPL and Apache-2.0) via PlatformIO's dependency manager.

If you are a rights holder and believe material in this repository infringes your copyright, please open an issue and it will be addressed promptly.
