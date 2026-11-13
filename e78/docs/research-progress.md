# Mapping, simulation and remaining calibration work

This is a sanitized summary of the originating research through 2026-11-12.
The image, captures, private traces and emulator databases are not distributed.
The [metadata catalog](../definitions/research-catalog.json) pins the exact
baseline and source-catalog digest; it does not make private experiments
independently reproducible without their inputs.

## Current definitions

The public metadata contains 108 records and no stored cell values. Records
include axes, scalars, model coefficient arrays and tables; they are not 108
fully validated editable maps. `export_research_catalog.py` reconstructs 15,893
logical cells from a caller-owned matching baseline and checks each record's
byte digest. Logical arrays may overlap and do not measure unique flash coverage.

The earlier ten-table worksheet catalog remains available. Later work adds
qualified torque/arbitration, DFCO, model and spark definitions. Exact input
scaling, selection, validity and physical-unit qualifications matter as much as
the address. Unresolved semantics are not filled in from a donor's table name.

| Recent work | Own-image result | Actual original-instruction scope |
| --- | --- | --- |
| Base spark | Headers `07394A/073FC2`, signed8, 33×33, 0.5 nominal degree/stored count; nominal RPM and charge axes | 3,831 isolated normalization/lookup/blend cases passed |
| VCP spark contribution | Headers `07463A/074AA2`, signed8, 33 charge rows×17 rate columns, 0.5 nominal degree/stored count | 3,719 isolated rate/lookup/selection/weighting cases passed; separate Python arithmetic reproduced each result |
| Pedal A/B/C draft | Matched bounded remapping, 378 cells and 1,030 bytes including checksums, physical block6 | Six builder tests and 14,837 interpolation checks in the originating package; portable tests reported separately |

The base-spark high/low role names do not assign either table to a specific AKI
rating. Its blend weight is an internal control value, not measured fuel octane.
Upper normalized input65535 is not65536; integer endpoint behavior matters.

VCP spark is a contribution to spark, not a desired cam-position table.
Numeric modes2/3 select the zero alternate; other tested modes select the primary.
Rate normalization uses inline raw endpoints3200/54400, giving nominal RPM
knots400..6800. The stored axis0784AC has matching display knots but is not read
by this lookup. Changing that stored axis does not change these inline endpoints.
The multiplier is not a verified cam-angle channel. Desired intake/exhaust cam
targets, actual tracking conventions and complete VVT control remain unresolved.
Donor humidity-table offsets did not have matching headers in this image.

The A/B/C example changes requested response, preserving original low/full pedal
and low-speed behavior. It does not increase maximum demand, establish a power
gain or provide a 92-AKI/crackle calibration. Its six-percentage-point bound is
an experimental shape choice, not an engine/transmission safety limit.

## What simulation established

- Ghidra executed selected original PowerPC fragments for table interpolation,
  units, torque arbitration, fuel-cut state and spark paths against synthetic RAM.
- Restricted eTPU models covered reviewed channel setup, phase calculations and
  event conditions. They do not execute the whole microengine or physical pins.
- Fake ECU peers and pseudo-terminal fixtures exercised host protocol sequencing,
  retry, timeout and cleanup behavior. These differ from physical CAN results.
- An experimental loader was exercised through modeled erase/write/readback and
  stock bootloader return. Peripheral/MMIO, timing, interrupts and restoration
  limitations remain explicit; this package ships no such loader binary.
- GM-ECU-Simulator work used headless protocol/importer tests, not a full stock
  CPU/eTPU/combustion model. Vendor eTPU simulation was researched, not run.

The public package includes host protocol tests and definition reconstruction,
not every private Ghidra/eTPU harness. There is no simulated horsepower, knock
margin, fuel economy, exhaust temperature or durability result.

## Before a validated power calibration

Cam target/feedback interpretation, complete selected boost and fueling targets,
valid loaded measurements and relevant health evidence remain open. Existing
drives had limited RPM coverage and predated a later hardware change; commanded
ratio is not measured mixture, spark advance is not knock correction, and an
ECU torque estimate is not independently measured power.

No justified aggressive92-AKI boost, timing or fuel targets have been selected.
Cam targets remain stock for the first candidate. Physical write/recovery is a
separate integration requirement. `flash_ready=false` and
`complete_aggressive_power_tune=false` remain accurate.
