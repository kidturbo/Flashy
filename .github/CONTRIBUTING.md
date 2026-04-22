# Contributing to Flashy

Contributions are welcome — firmware improvements, Python tools, documentation, and clean-room kernels.

## Prerequisites

- [PlatformIO CLI](https://platformio.org/install/cli) or VS Code + PlatformIO extension
- Python 3.8+ (for host-side tools)
- Adafruit Feather M4 CAN Express (for firmware testing)
- Git

## Building the firmware

```bash
pio run                    # compile
pio run -t upload          # flash to Feather via USB
pio run -t clean           # clean build artifacts
```

Config is in `platformio.ini` — target board `adafruit_feather_m4_can`, framework `arduino`. Dependencies are auto-managed by PlatformIO.

## Kernel binary handling

The repo does **not** ship proprietary kernel binaries. The headers `src/gm_kernels.h`, `src/e67_kernel.h`, and `src/t87a_kernel.h` are stubs that use `__has_include` to pull in a `*_private.h` file you supply.

- **Never commit `*_private.h` files** — they are in `.gitignore`
- Two paths to get kernels:
  1. Extract from a CAN capture of a tool you own (see `tools/extract_kernel*.py`)
  2. Build the clean-room Kernels from `Cernels/` (MIT-licensed, public docs only)
- If your PR touches kernel upload logic, test with a confirmed-working kernel on real hardware

## Testing phases

| Phase | What | Hardware needed |
|-------|------|-----------------|
| **A** | Serial echo — verify command parser compiles and responds correctly | Feather M4 (USB serial only, no CAN) |
| **B** | SavvyCAN + CAN adapter — verify ISO-TP/UDS interoperability | Feather M4 + CAN bus adapter |
| **C** | Real vehicle OBD-II — full end-to-end test | Feather M4 + ECU (bench or vehicle) |

Phase A is the minimum for any PR. Phase C is required for changes that affect flash read/write behavior.

## Python tools

- Located in `tools/`
- Key dependencies: `pyserial` (all tools), `pycryptodome` (gm5byte only)
- Auto-detect Feather COM port: `python tools/detect_port.py`
- Build portable .exe distribution: `python tools/build_exe.py` (uses PyInstaller, outputs to `dist/J2534-Tools/`)

## Code conventions

- **Hybrid C/C++:** pure C for logic modules (`uds.c`, `seed_key.c`, isotp), C++ for Arduino/CAN shims
- **`extern "C"`** wrappers in headers for C linkage
- **Default CAN baud:** 500 kbps, standard IDs 0x7E0/0x7E8
- **Serial output prefixes:** `OK:`, `ERR:`, `WARN:`, `INFO:` for machine-parseable output

## Pull request process

1. Fork the repo and branch from `main`
2. Describe what module/command is affected
3. Include serial log output showing the change works (Phase A minimum)
4. Do **not** include `.csv`, `.log`, `.bin`, or `*_private.h` files
5. Keep PRs focused — one feature or fix per PR

## Clean-room Kernel contributions (Cernels/)

- Must be written solely from public documentation (Freescale/NXP reference manuals, ST datasheets)
- No disassembly of commercial kernels may be used as reference
- MIT licensed
- Build with NXP `powerpc-eabivle-gcc` toolchain
- See `Cernels/README.md` for build instructions and per-ECU status
