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

## Running the Python Tools (Windows, no VS Code)

Most users never need to touch a Python script — the prebuilt `.bat` launchers in [Option A](#option-a--pre-built-easy) cover VIN scan, read, write, capture, and flash firmware. But the [`tools/`](https://github.com/kidturbo/Flashy/tree/main/tools) folder has more — OBD-II Clear DTC, Mode 9 dump, RTC sync, and the E92A authentication helpers — that aren't in the bundled `.exe` set. Three ways to run them, easiest first:

### 1. Windows Terminal (recommended for most)

The cleanest no-IDE option:

1. Install [Python 3.9+](https://www.python.org/downloads/windows/) — **check "Add to PATH"** during install.
2. `pip install pyserial pycryptodome`
3. Get [Windows Terminal](https://aka.ms/terminal) from the Microsoft Store (already on Win 11).
4. Right-click your `Flashy` folder → **Open in Terminal** → run:
   ```
   python tools/obd2/scan_full.py --port COM13 --json
   python tools/set_rtc_local.py
   python tools/obd2/clear_dtc.py --port COM13
   ```

[Git Bash](https://git-scm.com/download/win) works the same way if you prefer a Unix-style shell.

### 2. Thonny — single-download Python IDE

If you want a click-to-run editor without VS Code's complexity, [Thonny](https://thonny.org/) is one download, opens your repo, has a Run button, and includes a built-in shell. Good for someone new to Python.

### 3. PyCharm Community — full IDE (closest to VS Code)

[PyCharm Community Edition](https://www.jetbrains.com/pycharm/download/) is a full professional IDE — free for open-source and personal use. Closest like-for-like swap with VS Code in feature set, just JetBrains-flavored. Run buttons, debugger, terminal, and project view all built in.

### For maintainers — what we use

The repo includes a [`.vscode/extensions.json`](https://github.com/kidturbo/Flashy/blob/main/.vscode/extensions.json) recommendation. When opening the repo in VS Code, accept the "Install recommended extensions" prompt to get:

- **[PlatformIO IDE](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)** — build / upload firmware, serial monitor.
- **[Python (Microsoft)](https://marketplace.visualstudio.com/items?itemName=ms-python.python)** — run / debug `.py`.

The full per-tool catalog and one-line descriptions live on the [Tools Reference](https://kidturbo.github.io/Flashy/Tools-Reference.html) page.

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
- [Tools Reference](https://kidturbo.github.io/Flashy/Tools-Reference.html) — every Python helper with a one-line description, including the cross-vendor [`tools/obd2/`](https://github.com/kidturbo/Flashy/tree/main/tools/obd2) Clear DTC and Mode 9 scan utilities
- [Report a Bug](https://github.com/kidturbo/Flashy/issues/new?template=bug_report.md) — if something goes wrong
- [Request new module support](https://github.com/kidturbo/Flashy/issues/new?template=feature_request.md) — for BCMs or any CAN node not yet on the list
