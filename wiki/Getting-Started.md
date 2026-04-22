# Getting Started

Once your [hardware is assembled](Hardware-Assembly), flash the firmware and connect. Flashy can interact with any CAN-based automotive module — ECMs, TCMs, BCMs, and more — using diagnostic services, reversing workflows, and flash programming routines.

## Option A — Pre-built (Easy)

1. Download the latest [**Flashy-Tool.zip**](https://github.com/kidturbo/Flashy/releases/latest)
2. Extract it somewhere convenient (no install required)
3. Plug the Feather into your PC via USB
4. Double-click `firmware.uf2` — the Feather will reboot as a USB drive called `FEATHERBOOT`. Drag the `.uf2` onto it and it will flash itself automatically.
5. Run `detect_port.exe` to find which COM port your Feather is on
6. Use the `.bat` launcher scripts for common operations (read, write, VIN scan, etc.)

## Option B — Build from Source

Requires [PlatformIO](https://platformio.org/install/cli) and Python 3.8+.

```bash
git clone https://github.com/kidturbo/Flashy.git
cd Flashy
pio run -t upload
```

PlatformIO auto-detects the Feather and flashes it. See [CONTRIBUTING.md](https://github.com/kidturbo/Flashy/blob/main/.github/CONTRIBUTING.md) for full build details.

---

## First Serial Connection

Open a terminal (PuTTY, TeraTerm, Arduino IDE Serial Monitor) at **115200 baud** on the Feather's COM port.

You should see the Flashy banner when the board boots or you press reset:

```
Flashy J2534 Pass-Thru vX.Y.Z
Type HELP for commands, MENU for interactive mode
>
```

Type `MENU` for the interactive module-select interface, or type commands directly.

---

## Your First Read

With the CAN bus connected (key in RUN for OBD-II, or module powered up on a bench harness), the easiest way to do your first read is the interactive **MENU**:

```
MENU
```

The menu walks you through:

1. **Pick your module** — E38, E67, T87, T87A, etc. (Flashy auto-sets the CAN IDs and seed-key algorithm for you)
2. **Pick an action** — read, write, diagnostics, or raw UDS
3. **Pick a destination** — stream the read to the SD card or back to the PC

CAN is already initialized at boot (AUTOINIT) and the baud is auto-detected, so you don't need to run `INIT` or `SETID` first.

See the [ECU Reference Guide](https://kidturbo.github.io/Flashy/ecu-reference.html) for the full list of supported modules.

The `FULLREAD` will stream the module's flash to the SD card. Expect a few minutes depending on flash size and connection speed.

> **Prefer typing commands directly?** See [Advanced: Direct Commands](Advanced-Commands) for the raw `INIT` / `ALGO` / `AUTH` / `FULLREAD` workflow and a full command reference.

---

## LED Status Codes

The NeoPixel on the Feather shows the current state:

| Color | Meaning |
|-------|---------|
| **Off** | Idle, waiting for commands |
| **Green** | Connected to module, authenticated |
| **Blue** | Reading flash |
| **Yellow** | Writing flash |
| **Purple** | BAM mode — waiting for module reset |
| **Red** | Error — check serial output for details |
| **Strobe** | Success celebration |

---

## Next Steps

- [Downloads](Downloads) — what's in the release zip
- [Documentation Index](Documentation-Index) — full protocol, module reference, unlock recipes
- [Report a Bug](https://github.com/kidturbo/Flashy/issues/new?template=bug_report.md) — if something goes wrong
- [Request new module support](https://github.com/kidturbo/Flashy/issues/new?template=feature_request.md) — for BCMs or any CAN node not yet on the list
