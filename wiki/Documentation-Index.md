# Documentation Index

All the deep technical documentation lives at [kidturbo.github.io/Flashy](https://kidturbo.github.io/Flashy/), served as styled HTML pages via GitHub Pages.

## Reference Pages

| Page | What it covers |
|------|---------------|
| [ECU Reference Guide](https://kidturbo.github.io/Flashy/ecu-reference.html) | MCU specs, memory maps, and verification status for every supported module — E38, E67, E92, T87A, and more. Start here if you're curious whether your ECU is supported. |
| [J2534 CAN Bus Reference](https://kidturbo.github.io/Flashy/J2534-CANbus-Reference.html) | Full J2534 Pass-Thru protocol reference — CAN IDs, ISO-TP framing, UDS service list, error codes |
| [Read / Write Instructions](https://kidturbo.github.io/Flashy/Read-Write-Instructions.html) | Step-by-step usage guide for every read/write workflow, with serial command examples |
| [T87A Unlock Recipe](https://kidturbo.github.io/Flashy/T87A-Unlock-Recipe.html) | The 5-patch dual-unlock technique (USBJTAG signature bypass + HPT unlock), flash memory map, verified OS versions |
| [Changelog](https://kidturbo.github.io/Flashy/changelog.html) | Version history and release notes |

## Wiki Pages

| Page | What it covers |
|------|---------------|
| [Home](Home) | Project overview and navigation |
| [Hardware Assembly](Hardware-Assembly) | Building the Feather + SD Wing stack, breaking the 120Ω terminator, OBD-II wiring |
| [Getting Started](Getting-Started) | Firmware flash, first serial connection, LED codes, first ECU read |
| [Advanced: Direct Commands](Advanced-Commands) | Raw serial command reference for scripting / automation |
| [Downloads](Downloads) | What's in each release, system requirements |

## Source Code

- **Firmware:** [src/](https://github.com/kidturbo/Flashy/tree/main/src) — Arduino/PlatformIO project
- **Python tools:** [tools/](https://github.com/kidturbo/Flashy/tree/main/tools) — host-side scripts
- **Clean-room Kernels:** [Cernels/](https://github.com/kidturbo/Flashy/tree/main/Cernels) — MIT-licensed PowerPC kernel sources

## Contributing & Support

- **Bug reports:** [Open an issue](https://github.com/kidturbo/Flashy/issues/new?template=bug_report.md) using the bug template
- **Feature requests:** [Open an issue](https://github.com/kidturbo/Flashy/issues/new?template=feature_request.md) using the feature template
- **Contributing:** See [CONTRIBUTING.md](https://github.com/kidturbo/Flashy/blob/main/.github/CONTRIBUTING.md) for build and PR guidelines
- **Release process:** See [RELEASING.md](https://github.com/kidturbo/Flashy/blob/main/docs/RELEASING.md)
