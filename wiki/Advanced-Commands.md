<sub>Mirrored from [`wiki/Advanced-Commands.md`](https://github.com/kidturbo/Flashy/blob/main/wiki/Advanced-Commands.md) by the [sync workflow](https://github.com/kidturbo/Flashy/actions/workflows/sync-wiki.yml). Direct edits in the GitHub Wiki UI will be overwritten on the next push to `main` — [edit upstream](https://github.com/kidturbo/Flashy/edit/main/wiki/Advanced-Commands.md) instead.</sub>

# Advanced: Direct Commands

The [MENU](Getting-Started) flow is the recommended path for most users. But if you'd rather drive Flashy with direct serial commands — for scripting, automation, or just preference — this page covers the raw command workflow.

## The Old-School Read Sequence

Before the MENU system and AUTOINIT, reading a module looked like this:

```
INIT                   # Initialize the CAN bus
ALGO <module-type>     # Select module (sets CAN IDs + seed-key algo)
AUTH                   # SecurityAccess (seed -> auto-compute key)
FULLREAD               # Stream full flash to SD card
```

AUTOINIT now handles the `INIT` step automatically at boot — the firmware brings up CAN and auto-detects the baud rate. But the command still exists if you need to reinitialize, change baud, or troubleshoot.

## Full Command Reference

| Command | Purpose |
|---------|---------|
| `INIT [baud]` | Init CAN bus (default 500000 — normally handled by AUTOINIT) |
| `SETID <tx> <rx>` | Set tester / ECU CAN IDs (hex) |
| `ALGO <type>` | Select module + seed-key algo (e38, e67, t87, t87a, e92, etc.) |
| `DIAG [session]` | DiagnosticSessionControl |
| `AUTH [key_hex]` | SecurityAccess (seed -> auto-compute key) |
| `KERNEL` | Upload kernel to ECU |
| `FULLREAD` | Automated full flash read |
| `CALREAD` | Calibration-only read (E38) |
| `READ <addr> <blocks>` | Manual block read |
| `WRITE <addr> <blocks>` | Write blocks from PC |
| `CALWRITE` | Automated cal write + verify |
| `FULLWRITE` | Automated full flash write + verify |
| `BAMREAD` | T87A: BAM boot-mode full read |
| `BAMWRITE [file]` | T87A: BAM boot-mode write + verify |
| `VIN` | Read VIN |
| `VINWRITE <vin>` | Write 17-character VIN |
| `SCAN` / `SCAN FAST` | Scan bus for all modules + VINs (default) |
| `SCAN FULL` | Mode 9 dump per module — VIN, ECU Name, CAL IDs, CVNs, GM `$1A B4` |
| `OSID` | Walk every module on the bus and print primary OSID per responder |
| `PING` | TesterPresent probe — reports alive (positive or NRC) for active CAN IDs |
| `CLEARDTC` | Universal Clear DTC (Mode `$04` -> 0x7DF, then UDS `$10 03` + `$14`, then GMLAN wake-up + `$14` if needed) |
| `E92ID` | Classify attached E92 as Early/Late from PN cluster + VIN model year |
| `E92SAPROBE` | Sweep E92 SecurityAccess sub-levels (diagnostic) |
| `E92FULLREAD` | Full 4 MB read on Late E92A: unlock + kernel upload + read + reset |
| `KLIST` | List kernels compiled into firmware (registry) |
| `KUSE <id>` | Select kernel by ID for next operation |
| `SET <key> <val>` | Per-kernel runtime override (boot delay, probe svc/PID, TXE, $3E) |
| `PARAMS` | Print current SET overrides |
| `SETCLOCK YYMMDD HHMM[SS]` | Set the PCF8523 RTC (drives SD log filenames) |
| `ERASE <addr> <size>` | Erase flash region |
| `RESET [type]` | ECU Reset |
| `RAW <hex>` | Send raw UDS request |
| `CANSEND <id> <hex>` | Send raw CAN frame |
| `CAPTURE <ms>` | Stream raw CAN frames as SavvyCAN-format CSV for `<ms>` ms |
| `STATUS` | Show connection state |
| `MENU` | Enter interactive mode |
| `HELP` | List commands |

For protocol-level details (ISO-TP framing, UDS service IDs, error codes), see the [J2534 CAN Bus Reference](https://kidturbo.github.io/Flashy/J2534-CANbus-Reference.html).

## Scripting

The Python tools in `tools/` drive these commands over USB serial for automation — see the scripts in the repo for patterns, or the [Read / Write Instructions](https://kidturbo.github.io/Flashy/Read-Write-Instructions.html) for worked examples.

For vendor-neutral OBD-II / UDS diagnostics that work across manufacturers, see [`tools/obd2/`](https://github.com/kidturbo/Flashy/tree/main/tools/obd2) — host wrappers around `CLEARDTC` and `SCAN FULL` with parsed output, including `--json` for `scan_full.py`. The full tool catalog is in the [Tools Reference](https://kidturbo.github.io/Flashy/Tools-Reference.html).
