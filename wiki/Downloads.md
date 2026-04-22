# Downloads

All releases are published at: **[github.com/kidturbo/Flashy/releases](https://github.com/kidturbo/Flashy/releases)**

The latest stable release is always at: **[github.com/kidturbo/Flashy/releases/latest](https://github.com/kidturbo/Flashy/releases/latest)**

## What's in `Flashy-Tool.zip`?

The release zip is portable — no installer, no Python required, no admin rights needed. Extract it anywhere and run the .bat launchers or .exe tools directly.

| File | Purpose |
|------|---------|
| `firmware.uf2` | Feather M4 firmware — drag-and-drop to the FEATHERBOOT drive to flash |
| `detect_port.exe` | Auto-find the Feather's COM port |
| `capture_read.exe` | Drive a `FULLREAD` and save to a `.bin` file on the PC |
| `capture_write.exe` | Drive a full-flash write from a `.bin` file |
| `vin_scan.exe` | Scan the CAN bus for all responding modules + their VINs |
| `vin_update.exe` | Write a new 17-character VIN to a module |
| `t87a_patch.exe` | T87A 5-patch dual-unlock + checksum recalculator |
| `seed_key_algo.exe` | Compute GM seed-key responses (all 1280 algorithms) |
| `Read-Write-Instructions.html` | Offline copy of the usage guide |
| `*.bat` launchers | One-click scripts for common operations |

## System Requirements

- **OS:** Windows 10 or later (x64)
- **Hardware:** Assembled Flashy device (see [Hardware Assembly](Hardware-Assembly))
- **Space:** ~50 MB extracted

## Using a Release

1. Download `Flashy-Tool.zip`
2. Extract to a folder (e.g., `C:\Flashy\`)
3. Plug Feather into USB and flash `firmware.uf2`
4. Open a command prompt in the extracted folder
5. Run `detect_port.exe` to find your COM port
6. Launch any of the `.bat` scripts

See [Getting Started](Getting-Started) for first-run details.

---

## Version History

Each release's changelog is attached to the GitHub release notes. For a full history, see the [Changelog](https://kidturbo.github.io/Flashy/changelog.html) page.
