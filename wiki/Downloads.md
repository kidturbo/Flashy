<sub>Mirrored from [`wiki/Downloads.md`](https://github.com/kidturbo/Flashy/blob/main/wiki/Downloads.md) by the [sync workflow](https://github.com/kidturbo/Flashy/actions/workflows/sync-wiki.yml). Direct edits in the GitHub Wiki UI will be overwritten on the next push to `main` — [edit upstream](https://github.com/kidturbo/Flashy/edit/main/wiki/Downloads.md) instead.</sub>

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

## T87A Patched Library

`T87A-Patched-Library-v1.zip` — 11 full 4 MB T87A TCM flash images with the 5-patch USBJTAG + HPT unlock recipe pre-applied. Includes the recipe documentation and README.

**Included OSes** (bench status reflects Flashy validation):

| OS PN | Trans | Status |
|---|---|---|
| **24288836** | 10L80/10L90 | **Verified** |
| **24293216** | 10L80/10L90 | **Verified (BAM)** |
| 24281243 | TBD | Untested |
| 24283721 | 8L45 (CT6) | Untested |
| 24285377 | TBD | Untested |
| 24286913 | TBD | Untested |
| 24286985 | TBD | Untested (per-OS offsets) |
| 24288259 | 10L | Untested |
| 24288574 | TBD | Untested |
| 24288835 | 8L90 | Untested |
| 24291283 | 8L90 (CTSV) | Untested |

See [T87A-Unlock-Recipe.html](https://kidturbo.github.io/Flashy/T87A-Unlock-Recipe.html) (also bundled in the zip) for the full catalog with CS1/CS2, CRC32, and cross-OS write compatibility matrix.

**Write path guidance:**
- JTAG / BAM — cross-OS safe, flash any bin
- Flashy HS Full Write — **same-OS only** (firmware aborts mismatches in v1.4.5+)
- Flashy HS Cal Write — same-OS cal edits

---

## Version History

Each release's changelog is attached to the GitHub release notes. For a full history, see the [Changelog](https://kidturbo.github.io/Flashy/changelog.html) page.
