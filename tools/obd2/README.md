# OBD-II / UDS diagnostic tools

Vendor-neutral host scripts for the OBD-II services Flashy ships
universally. These wrap the firmware's `CLEARDTC` and `SCAN FULL`
serial commands and parse their output for scripting.

## Scripts

| Script | What it does |
|---|---|
| [`clear_dtc.py`](clear_dtc.py) | Universal Clear DTC — Mode $04 broadcast on `0x7DF`, then per-responder UDS `$10 03` + `$14 FF FF FF` fallback. |
| [`scan_full.py`](scan_full.py) | For every module on the bus, dump VIN, ECU Name, CAL IDs, CVNs, and the GM `$1A B4` cal-info string. JSON output available. |

Both require `pyserial`:

```bash
pip install pyserial
```

## Usage

```bash
# clear DTCs on every responding module
python tools/obd2/clear_dtc.py --port COM13

# full Mode 9 dump of everything on the bus, save as JSON
python tools/obd2/scan_full.py --port COM13 --json > bench.json
```

`--baud` overrides the default 500000 if you need it.

## What "universal" actually means

OBD-II / SAE J1979 mandate a small set of services every road-legal
vehicle (1996+ in the US) supports:

| Mode | Purpose | Used by |
|---|---|---|
| `$03` | Read stored DTCs | `READDTC` |
| `$04` | Clear DTCs | `CLEARDTC` Pass 1 |
| `$09 PID 02` | VIN | `SCAN`, `SCAN FULL` |
| `$09 PID 04` | Calibration IDs (CAL IDs) | `SCAN FULL` |
| `$09 PID 06` | Calibration Verification Numbers (CVNs) | `SCAN FULL` |
| `$09 PID 0A` | ECU Name | `SCAN FULL` |

These are addressed via the **functional ID `0x7DF`** (broadcast). Every
J1979-compliant ECU on the bus listens to it and answers on its own
physical RX in the `0x7E8..0x7EF` range.

UDS / ISO 14229 services like `$14` (ClearDiagnosticInformation) are
**physical-addressed** — `clear_dtc.py` sends them P2P to each
responder's physical TX (`0x7E0..0x7E7`) after the broadcast pass.

## NRC reference

When an ECU rejects a request you'll see `NRC 0xXX`. The most common
ones for Clear DTC / session control:

| NRC | Name | Common cause |
|-----|------|--------------|
| `0x11` | serviceNotSupported | Module doesn't speak this service in current session. GM modules in default session reject most UDS services — needs `$28` + `$A2` + `$A5` programming wakeup first. |
| `0x12` | subFunctionNotSupported | Wrong sub-function, e.g. `$10 03` requested but module only supports `$10 01` / `$10 02`. |
| `0x22` | conditionsNotCorrect | Most common rejection on Mode `$04`. ECU thinks ignition isn't in RUN, engine state is wrong, or DTC is currently active and not clearable. |
| `0x33` | securityAccessDenied | Need `$27` SecurityAccess unlock first (rare for clear, common for write). |
| `0x78` | responsePending | ECU is working on it — wait for the real reply. The firmware handles this transparently. |
| `0x7E` / `0x7F` | …NotSupportedInActiveSession | Wrong session. Open `$10 03` (extended) or `$10 02` (programming) first. |

## Bench notes

This bench (E92 ECM + FPCM at `0x7E3`) returned NRC `0x22` on Mode `$04`
in the documented test (2026-04-25) — a known GM quirk where ECMs in
default session refuse Mode 4. The mechanism is correct; the result is
the ECU's internal state. On a road-legal vehicle with engine off and
ignition fully ON, Pass 1 typically clears cleanly without falling
through to Pass 2.

## See also

- [Tools Reference](../../docs/Tools-Reference.html) — full tool index
- [Read-Write-Instructions](../../docs/Read-Write-Instructions.html) — main usage guide
