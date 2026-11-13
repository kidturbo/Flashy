# Worked exact-image profile

Applicability requires the exact baseline SHA-256 `3b9b58421977ae2a5fb0f5d4d1bf71702cd97922109a215cab1ba9405e3920ba`, 3 MiB, OS12644082 and EngineOper55581437. Matching the OS number alone is insufficient. These are research definitions, with incomplete runtime/loaded validation; `editable:false`, `flash_ready:false`. The explicit experimental pedal builder is narrower than a general editor and does not change that evidence boundary.

Calibration is 0x020000–0x07FFFF: System55582024, Fuel55580794, Speedometer55580796, EngineDiag55582022, EngineOper55581437. OS begins0x080000. The whole first512KiB is not bootloader.

## Ten static table definitions

All primary matrices and axes are big-endian float32, factor1/offset0, row-major with columns changing fastest; endpoint clamp and linear/bilinear lookup are code-derived.

| Table | Data offset | Shape | Row / column axis offsets | Output |
| --- | --- | --- | --- | --- |
| Maximum boost-control pressure limit | 04DFB4 | 7 | — / 04E4F0 temperature | kPa absolute |
| Indicated peak torque | 0534A4 | 4×16 | 053B10 percentage / 053AD0 RPM | Nm |
| Maximum brake torque | 0535A4 | 4×16 | 053B10 / 053AD0 | Nm |
| Overboost indicated torque | 053464 | 16 | — / 053AD0 | Nm |
| Pedal progression A | 046944 | 17×17 | 047C1C pedal% / 047D0C km/h | kW selected mode1 |
| Pedal progression B | 046DC8 | 17×17 | Same | Same |
| Pedal progression C | 04724C | 17×17 | Same | Same |
| Pedal fallback | 0476D0 | 17×17 | Same | Same on traced mode1 path |
| Barometric power multiplier | 046664 | 17×5 | 047B54 kW / 047D50 kPa absolute | BEu16 /32768 |
| Pedal/RPM modifier | 04675C | 9×9 | 047B98 pedal% / 047CE8 RPM | BEs16 /32 |

There are1473 cells across these ten tables. Complete values are exported only from a private exact original by the optional exporter; they are not shipped as firmware/table dumps. The historical source evidence is cataloged in ../docs/evidence-ledger.json.

A/B/C are identical in the original; dispatcher and retained-state gates matter. Fallback and transitions remain incompletely validated. Selected power converts through `30000/(pi*max(filtered_RPM,300))` plus conditioning and runtime affine drivetrain factors. Do not call raw map kW measured power or interpret downstream axle requests as crankshaft torque.

Torque rows use percentage axis[0,20,60,80]; a ROM-selected branch normally writes row0, not a measurement of ethanol. Boost pressure is absolute and is a cap in a larger controller, not a wastegate-duty or guaranteed manifold target. The barometric modifier is all unity in this image. The reference name “low-speed” does not override the own-code RPM column and threshold gate for the other modifier.

## Later non-editable research

The minimum-spark main structure header07895A/data07895E is33rows×17columns, signed16BE, numerical factor1/128. Fallback and downstream clamp sites are traced, but final overrun spark selection is unresolved. DFCO delay structures have own-code state selectors2/3, with scheduler/time units and complete fuel-cut chain still unresolved. See ../docs/discoveries.md for detailed scope and rejected pattern matches. Neither set is an editable crackle definition.

## Current metadata extension

[research-catalog.json](research-catalog.json) now records108 entries, including
axes/scalars/models and two recent base-spark plus two VCP spark contributions.
It contains no original cell arrays. The new exporter reconstructs15,893 logical
cells from the private matching baseline, preserving non-editable status.
[Current interpretation and limits](../docs/research-progress.md) supersede the
older final-spark/DFCO knowledge boundaries above; full vehicle validation remains
open. The VCP rate axis uses inline endpoints and only shares display knots with
the existing stored rate axis.
