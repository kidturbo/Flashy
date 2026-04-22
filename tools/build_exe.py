#!/usr/bin/env python3
"""
build_exe.py — Build standalone .exe files for the J2534 Pass-Thru toolset.

Creates a portable 'dist/J2534-Tools/' folder containing:
  - capture_read.exe    (ECU flash reader — E38/T87)
  - capture_write.exe   (E38 flash writer — cal or full)
  - t87_calwrite.exe    (T87 TCM calibration writer)
  - t87_fullwrite.exe   (T87 TCM full flash writer)
  - vin_update.exe      (single-module VIN updater)
  - vin_scan.exe        (multi-module VIN scanner)
  - detect_port.exe     (Feather port detector)
  - Batch file shortcuts
  - Pre-compiled firmware .uf2 (if available)
  - Read-Write-Instructions.html

Usage:
    python build_exe.py          # build all tools
    python build_exe.py --clean  # clean build artifacts first
    python build_exe.py --zip    # build + create distribution .zip

Requires: pip install pyinstaller pyserial
"""

import os
import shutil
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(TOOLS_DIR)
DIST_DIR = os.path.join(PROJECT_DIR, 'dist', 'Flashy-Tool')
BUILD_DIR = os.path.join(PROJECT_DIR, 'build_pyinstaller')

# Tools to build: (script_name, exe_name, extra_hidden_imports, extra_datas)
#
# This release ships only tools that work without a proprietary kernel
# embedded in the firmware. Kernel-dependent tools (capture_read,
# capture_write, t87_calwrite, t87_fullwrite) are not included — they
# require the user to supply their own kernel headers and rebuild
# firmware locally. See CONTRIBUTING.md for that workflow.
TOOLS = [
    ('detect_port.py', 'detect_port', [], []),
    ('vin_update.py', 'vin_update', ['keylib'], [
        (os.path.join(TOOLS_DIR, 'gm5byte', 'keylib.py'), 'gm5byte'),
    ]),
    ('vin_scan.py', 'vin_scan', ['keylib'], [
        (os.path.join(TOOLS_DIR, 'gm5byte', 'keylib.py'), 'gm5byte'),
    ]),
    ('capture_bus.py', 'capture_bus', [], []),
]

# Batch file wrappers for the dist package
#
# This release ships only kernel-independent launchers. Read/write
# launchers for E38/E67/T87/T87A are intentionally omitted because
# they require kernels that aren't shipped publicly.
BATCH_FILES = {
    'Flashy_Menu.bat': r'''@echo off
set "FLASHY_FROM_BAT=1"
title Flashy - Pick a Tool
:menu
cls
echo ==========================================
echo   Flashy J2534 Pass-Thru
echo ==========================================
echo.
echo   1) Detect Feather port
echo   2) VIN scan (find all modules on bus)
echo   3) VIN update (write VIN to a module)
echo   4) Capture CAN bus (30 sec to CSV)
echo   5) Flash firmware
echo.
echo   H) Open Read/Write Instructions
echo   Q) Quit
echo.
set /p choice="Choice: "
if "%choice%"=="1" call "%~dp0Detect_Port.bat"
if "%choice%"=="2" call "%~dp0VIN_Scan.bat"
if "%choice%"=="3" call "%~dp0VIN_Update.bat"
if "%choice%"=="4" call "%~dp0Capture_CAN_Bus.bat"
if "%choice%"=="5" call "%~dp0Flash_Firmware.bat"
if /i "%choice%"=="H" start "" "%~dp0Read-Write-Instructions.html"
if /i "%choice%"=="Q" exit
goto menu
''',
    'VIN_Scan.bat': r'''@echo off
set "FLASHY_FROM_BAT=1"
title VIN Scanner - All Modules
echo ==========================================
echo   VIN Scanner - Scan All CAN Bus Modules
echo ==========================================
echo.
"%~dp0vin_scan.exe"
echo.
pause
''',
    'VIN_Update.bat': r'''@echo off
set "FLASHY_FROM_BAT=1"
title VIN Update Tool
echo ==========================================
echo   VIN Update Tool
echo ==========================================
echo.
"%~dp0vin_update.exe"
echo.
pause
''',
    'Capture_CAN_Bus.bat': r'''@echo off
set "FLASHY_FROM_BAT=1"
title CAN Bus Capture (30 seconds)
echo ==========================================
echo   CAN Bus Capture - Streams all frames to a CSV
echo ==========================================
echo.
echo Default: 30-second capture, output saved to your Desktop as
echo          capture-YYYYMMDD-HHMMSS.csv (SavvyCAN format).
echo.
echo Useful for:
echo   - Recording any CAN-capable tool's session for protocol study
echo   - Capturing kernels other tools upload (then disassemble locally)
echo   - General CAN bus reverse engineering
echo.
echo Press Ctrl+C during capture to stop early.
echo.
pause
"%~dp0capture_bus.exe"
echo.
pause
''',
    'Detect_Port.bat': r'''@echo off
set "FLASHY_FROM_BAT=1"
title Feather Port Detector
echo ==========================================
echo   Feather M4 CAN Port Detector
echo ==========================================
echo.
"%~dp0detect_port.exe"
echo.
pause
''',
    'Flash_Firmware.bat': r'''@echo off
set "FLASHY_FROM_BAT=1"
title Flash Firmware to Feather M4 CAN
echo ==========================================
echo   Flash Firmware to Feather M4 CAN
echo ==========================================
echo.

if not exist "%~dp0firmware.uf2" (
    echo ERROR: firmware.uf2 not found in this folder.
    echo.
    echo Make sure firmware.uf2 is in the same folder as this batch file.
    echo.
    pause
    exit /b 1
)

echo Step 1: Double-click the RESET button on the Feather quickly.
echo         A USB drive called FEATHBOOT should appear.
echo.
echo Waiting for FEATHBOOT drive...
echo (Press Ctrl+C to cancel)
echo.

:wait_loop
for %%d in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%d:\INFO_UF2.TXT" (
        echo Found FEATHBOOT at %%d:\
        echo.
        echo Step 2: Copying firmware.uf2 to %%d:\...
        copy "%~dp0firmware.uf2" "%%d:\" >nul
        if errorlevel 1 (
            echo ERROR: Copy failed. Try again.
        ) else (
            echo.
            echo Done! Firmware flashed successfully.
            echo The Feather will reboot automatically.
        )
        echo.
        pause
        exit /b 0
    )
)
timeout /t 1 /nobreak >nul
goto wait_loop
''',
}

README_TEXT = r'''# Flashy — J2534 Pass-Thru Tools
## Portable toolset for automotive ECU diagnostics, reversing, and programming

### Quick Start
1. Plug in your Feather M4 CAN via USB
2. Double-click any .bat file to run a tool
3. The Feather COM port is auto-detected

### VIN Tools
| File | Description |
|------|-------------|
| VIN_Scan.bat | Scan CAN bus for all modules + read VINs |
| VIN_Update.bat | Update VIN on a single module |

### CAN Bus Capture
| File | Description |
|------|-------------|
| Capture_CAN_Bus.bat | 30-second CAN capture, saved to your Desktop as a SavvyCAN-format CSV |

The capture tool is useful for studying CAN traffic, recording any
CAN-capable tool's session, or grabbing kernels uploaded by other
tools so you can disassemble and analyze them locally.

### Utilities
| File | Description |
|------|-------------|
| Detect_Port.bat | Check if Feather is detected on USB |
| Flash_Firmware.bat | Auto-flash firmware.uf2 to Feather bootloader |

### Command-Line Usage
```
# VIN
vin_scan.exe                               # scan bus + read VINs
vin_scan.exe --scan-only                   # scan only
vin_update.exe                             # interactive VIN update
vin_update.exe --read                      # just read current VIN

# CAN bus capture (default 30 s, output to ~/Desktop/capture-<timestamp>.csv)
capture_bus.exe                            # 30-second capture
capture_bus.exe --duration 60000           # 60-second capture
capture_bus.exe --out myrun.csv            # custom output path
capture_bus.exe --port COM38               # specify COM port

# Utilities
detect_port.exe                            # find Feather's COM port
```

### Firmware
firmware.uf2 is included. Double-click the Feather's RESET button
twice to enter bootloader mode, then drag the .uf2 onto the FEATHBOOT
drive that appears. Or use Flash_Firmware.bat.

This release ships only kernel-independent tools. Flash read/write
for E38, E67, T87, and T87A modules requires user-supplied kernel
headers and a local firmware build (see CONTRIBUTING.md in the
source repo). The firmware's clean-room E92 read kernel works
out-of-the-box.

### Requirements
- Windows 10/11
- Adafruit Feather M4 CAN Express + AdaLogger FeatherWing (assembled)
- USB cable
- CAN bus connection (OBD-II or bench harness)

### Troubleshooting
- If auto-detect fails, specify the COM port manually with `--port COMx`
- Check Device Manager for the Feather's COM port number
- Make sure no other program (Arduino IDE, serial monitor) has the port open
- See Read-Write-Instructions.html for detailed documentation
'''


def clean():
    """Remove previous build artifacts."""
    for d in [BUILD_DIR, DIST_DIR]:
        if os.path.exists(d):
            print(f"  Removing {d}")
            shutil.rmtree(d)


def build_tool(script, exe_name, hidden_imports, datas):
    """Build a single tool with PyInstaller."""
    script_path = os.path.join(TOOLS_DIR, script)
    print(f"\n{'='*50}")
    print(f"  Building {exe_name}.exe from {script}")
    print(f"{'='*50}")

    hook_path = os.path.join(TOOLS_DIR, 'pyinstaller_hook.py')

    cmd = [
        sys.executable, '-m', 'PyInstaller',
        '--onefile',
        '--console',
        '--name', exe_name,
        '--distpath', DIST_DIR,
        '--workpath', BUILD_DIR,
        '--specpath', BUILD_DIR,
        '--clean',
        '--noconfirm',
        '--runtime-hook', hook_path,
    ]

    # Add hidden imports
    for imp in hidden_imports:
        cmd.extend(['--hidden-import', imp])

    # Add data files (for gm5byte keylib)
    for src, dest in datas:
        sep = ';' if sys.platform == 'win32' else ':'
        cmd.extend(['--add-data', f'{src}{sep}{dest}'])

    # Bundle detect_port.py for tools that import it
    if script != 'detect_port.py':
        detect_path = os.path.join(TOOLS_DIR, 'detect_port.py')
        sep = ';' if sys.platform == 'win32' else ':'
        cmd.extend(['--add-data', f'{detect_path}{sep}.'])
        cmd.extend(['--hidden-import', 'detect_port'])

    # Always include pyserial (with Windows list_ports backend)
    cmd.extend(['--hidden-import', 'serial'])
    cmd.extend(['--hidden-import', 'serial.tools'])
    cmd.extend(['--hidden-import', 'serial.tools.list_ports'])
    cmd.extend(['--hidden-import', 'serial.tools.list_ports_common'])
    cmd.extend(['--hidden-import', 'serial.tools.list_ports_windows'])

    cmd.append(script_path)

    result = subprocess.run(cmd, cwd=TOOLS_DIR)
    if result.returncode != 0:
        print(f"\n  *** FAILED to build {exe_name} ***")
        return False
    print(f"  OK: {exe_name}.exe")
    return True


def create_batch_files():
    """Create batch file shortcuts in dist."""
    print(f"\nCreating batch files...")
    for name, content in BATCH_FILES.items():
        path = os.path.join(DIST_DIR, name)
        with open(path, 'w', newline='\r\n') as f:
            f.write(content)
        print(f"  {name}")


def copy_firmware():
    """Copy firmware .uf2 if available."""
    uf2_dir = os.path.join(PROJECT_DIR, '.pio', 'build', 'feather_m4_can')
    uf2_file = os.path.join(uf2_dir, 'firmware.uf2')
    if os.path.exists(uf2_file):
        dest = os.path.join(DIST_DIR, 'firmware.uf2')
        shutil.copy2(uf2_file, dest)
        print(f"\nFirmware copied: firmware.uf2")
        return True
    else:
        print(f"\nNo firmware.uf2 found (build with 'pio run' first to include it)")
        return False


def copy_instructions():
    """Copy HTML instructions into dist."""
    html_src = os.path.join(PROJECT_DIR, 'docs', 'Read-Write-Instructions.html')
    if os.path.exists(html_src):
        dest = os.path.join(DIST_DIR, 'Read-Write-Instructions.html')
        shutil.copy2(html_src, dest)
        print(f"Instructions copied: Read-Write-Instructions.html")
        return True
    else:
        print(f"No Read-Write-Instructions.html found in dist/")
        return False


def create_readme():
    """Write README to dist."""
    path = os.path.join(DIST_DIR, 'README.md')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(README_TEXT)
    print(f"README.md created")


def create_zip():
    """Create a distribution .zip file."""
    import zipfile
    zip_name = 'Flashy-Tool'
    zip_path = os.path.join(PROJECT_DIR, 'dist', f'{zip_name}.zip')
    # Remove old zip if exists
    if os.path.exists(zip_path):
        os.remove(zip_path)
    print(f"\nCreating {zip_path}...")
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zf:
        for root, dirs, files in os.walk(DIST_DIR):
            for f in files:
                full_path = os.path.join(root, f)
                arcname = os.path.join(zip_name, os.path.relpath(full_path, DIST_DIR))
                zf.write(full_path, arcname)
    size_mb = os.path.getsize(zip_path) / 1024 / 1024
    print(f"  Created: {zip_path} ({size_mb:.1f} MB)")
    return zip_path


def create_installer():
    """Build a Windows installer .exe via Inno Setup. Requires Inno
    Setup 6 installed (https://jrsoftware.org/isinfo.php). Looks for
    ISCC.exe in the standard install paths and falls back to PATH."""
    candidates = [
        r'C:\Program Files (x86)\Inno Setup 6\ISCC.exe',
        r'C:\Program Files\Inno Setup 6\ISCC.exe',
        'iscc',
        'ISCC.exe',
    ]
    iscc = None
    for c in candidates:
        if os.path.isfile(c):
            iscc = c
            break
        else:
            try:
                subprocess.run([c, '/?'], capture_output=True, timeout=3)
                iscc = c
                break
            except Exception:
                continue
    if iscc is None:
        print('\n*** Inno Setup not found. Install from https://jrsoftware.org/isinfo.php')
        print('    Then re-run with --installer.')
        return None

    iss_path = os.path.join(PROJECT_DIR, 'installer', 'Flashy.iss')
    if not os.path.isfile(iss_path):
        print(f'\n*** Inno Setup script missing: {iss_path}')
        return None

    print(f'\nBuilding installer with Inno Setup ({iscc})...')
    result = subprocess.run([iscc, iss_path], cwd=os.path.dirname(iss_path))
    if result.returncode != 0:
        print('\n*** Installer build FAILED.')
        return None

    # Inno Setup writes to dist/Flashy-Tool-Setup-<version>.exe per OutputDir
    # in the .iss file. Find and report it.
    setup_glob = os.path.join(PROJECT_DIR, 'dist', 'Flashy-Tool-Setup-*.exe')
    import glob
    matches = sorted(glob.glob(setup_glob), key=os.path.getmtime, reverse=True)
    if matches:
        size_mb = os.path.getsize(matches[0]) / 1024 / 1024
        print(f'  Created: {matches[0]} ({size_mb:.1f} MB)')
        return matches[0]
    print('  WARNING: ISCC reported success but no installer .exe found.')
    return None


def main():
    if '--clean' in sys.argv:
        clean()

    os.makedirs(DIST_DIR, exist_ok=True)
    os.makedirs(BUILD_DIR, exist_ok=True)

    # Build each tool
    success = 0
    failed = 0
    for script, exe_name, hidden, datas in TOOLS:
        if build_tool(script, exe_name, hidden, datas):
            success += 1
        else:
            failed += 1

    # Create supporting files
    create_batch_files()
    copy_firmware()
    copy_instructions()
    create_readme()

    # Create reads/ and writes/ scaffold folders with READMEs.
    # These are the default local working dirs on the user's PC. They
    # are NOT tracked in git (see .gitignore) — anything dropped here
    # stays local.
    reads_dir = os.path.join(DIST_DIR, 'reads')
    os.makedirs(reads_dir, exist_ok=True)
    with open(os.path.join(reads_dir, 'README.md'), 'w', encoding='utf-8') as f:
        f.write(
            "# Reads folder\n\n"
            "This folder collects ECU flash dumps that come back over USB\n"
            "serial (PC-side reads). When you run a read tool from your PC\n"
            "and it streams the dump back over USB, the resulting `.bin`\n"
            "lands here.\n\n"
            "## Recommended path: read direct to the SD card\n\n"
            "For anything over ~512 KB the SD-card path on the AdaLogger\n"
            "FeatherWing is faster and more reliable. Reads stream straight\n"
            "to the card with no USB throughput bottleneck. To get them on\n"
            "your PC, pull the SD card and use a card reader.\n\n"
            "If you only ever read to SD, this folder will stay empty —\n"
            "that's fine.\n\n"
            "## Filename convention\n\n"
            "Reads are named `<VIN>_<OSID>.bin`, e.g.:\n\n"
            "    1G1ZJ577184252938_86YRJWK08086L16K.bin\n\n"
            "If VIN or OSID could not be read, those fields fall back to\n"
            "`UNKNOWN` or `NOOSID`.\n"
        )

    writes_dir = os.path.join(DIST_DIR, 'writes')
    os.makedirs(writes_dir, exist_ok=True)
    with open(os.path.join(writes_dir, 'README.md'), 'w', encoding='utf-8') as f:
        f.write(
            "# Writes folder\n\n"
            "Drop `.bin` files you intend to write to an ECU here. PC-side\n"
            "write tools default to looking in this folder for source files.\n\n"
            "## Recommended path: stage files on the SD card instead\n\n"
            "For best reliability with files over ~512 KB, copy the `.bin`\n"
            "to the SD card on the AdaLogger FeatherWing. The firmware\n"
            "creates a `Write/` directory on the SD card automatically and\n"
            "the MENU's write flow defaults to picking files from there.\n\n"
            "Pull the card, drop the file in `/Write/`, reinsert.\n\n"
            "## Safety reminder\n\n"
            "Keep your *original* read of an ECU pristine in `reads/` (or\n"
            "off-device entirely). Do all editing on a *copy*. Never write\n"
            "a `.bin` you can't reproduce — a failed write can leave a\n"
            "module unable to boot.\n"
        )

    print(f"\n{'='*50}")
    print(f"  Build complete: {success} succeeded, {failed} failed")
    print(f"  Output: {DIST_DIR}")
    print(f"{'='*50}")

    if failed == 0:
        if '--zip' in sys.argv or '--installer' in sys.argv:
            create_zip()
        if '--installer' in sys.argv:
            create_installer()
        if '--zip' not in sys.argv and '--installer' not in sys.argv:
            print(f"\nTo distribute: run 'python build_exe.py --zip' "
                  f"or '--installer' (needs Inno Setup 6).")
        print(f"\nRecipients can unzip and double-click any .bat file,")
        print(f"or run the Setup .exe (if --installer was used).")

    return 0 if failed == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
