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
| `SCAN` | Scan bus for all modules + VINs |
| `OSID` | Read OS / Calibration ID |
| `ERASE <addr> <size>` | Erase flash region |
| `RESET [type]` | ECU Reset |
| `RAW <hex>` | Send raw UDS request |
| `CANSEND <id> <hex>` | Send raw CAN frame |
| `STATUS` | Show connection state |
| `MENU` | Enter interactive mode |
| `HELP` | List commands |

For protocol-level details (ISO-TP framing, UDS service IDs, error codes), see the [J2534 CAN Bus Reference](https://kidturbo.github.io/Flashy/J2534-CANbus-Reference.html).

## Scripting

The Python tools in `tools/` drive these commands over USB serial for automation — see the scripts in the repo for patterns, or the [Read / Write Instructions](https://kidturbo.github.io/Flashy/Read-Write-Instructions.html) for worked examples.
