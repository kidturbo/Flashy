# E78 discoveries and reproducibility limits

This chapter summarizes research recorded on **2026-11-09 and 2026-11-10 UTC**.
The work progressed from a verified main-flash backup to exact-image code tracing,
bounded engine-off observations, offline candidate construction and isolated
firmware-instruction emulation. **Persistent flash writing, interrupted-write
recovery, loaded calibration validation and a complete tune remain unvalidated.**

The companion [evidence ledger](evidence-ledger.json) gives stable discovery IDs,
evidence classes, source-document paths and SHA-256 hashes. Those paths identify
private research records in the originating collection; they are provenance
labels, **not files supplied by this public repository or download links**.
Readers can reproduce the methods with their own data but cannot independently
inspect unpublished raw evidence from these labels alone. Source hashes identify
the document versions summarized here, not independent verification of a claim.

## Exact software scope

The analyzed installed image is 3,145,728 bytes, OS **12644082**, engine-operation
calibration **55581437**, SHA-256
`3b9b58421977ae2a5fb0f5d4d1bf71702cd97922109a215cab1ba9405e3920ba`.
Image addresses in this chapter are hexadecimal main-flash offsets unless marked
RAM. An OS number, model year or readable table is insufficient to transfer these
offsets to another file. Valid checksums and matching diagnostic CVNs establish
consistency of the installed image; factory-stock provenance is not established.
No original, donor or candidate binary, VIN, private identity report or raw
security/vehicle capture is distributed with this chapter.

Evidence classes used below:

- **Source inspection:** behavior established from code, headers or pinned references.
- **Offline test:** a host model or synthetic transport checks implementation.
- **Isolated instruction emulation:** selected original instructions execute with
  synthetic state; this is neither a booted ECU nor engine simulation.
- **Vehicle observation:** a bounded physical observation on this one target.
  Engine-off request values are not measured engine output.

## Connection, acquisition and restoration

**D01 — Classical CAN and the adapter dialect.** The tested RH02/Elmue SLCAN
combination received classical CAN at 500 kbit/s. Elmue silent open is `OS`;
bare `L` configures bus-load reporting and is not the usual SLCAN listen-only
command. Standard `slcand -l` or python-can listen-only initialization therefore
cannot be assumed equivalent. A corrected physical connection produced 63,037
valid frames in a 30-second silent capture. Tested vehicle-side wiring used OBD6
CAN H, OBD14 CAN L and OBD5 isolated bus-side ground, with adapter termination
OFF; this does not establish another adapter's wiring. Physical CAN FD and the
exact board revision remain unverified.

**D02 — Two matching original reads.** Two physical main-flash reads matched at
3,145,728 bytes and the digest above. The complete read operation took 225.34
seconds on the recorded setup. Coverage is `000000–2FFFFF`; 64 bytes at two
ECC-related holes were synthesized as `FF`, and shadow/password data are absent.
This is a main-flash backup, not a complete cloning image. Originals were
preserved and a second backup was hash-verified off the acquisition host.

**D03 — Loader state and restoration are distinct capabilities.** The successful
reader pins KernelMPC5566 Version 1.1, source revision
`320adc3a18985aff5614f2cd2ce5fc40ac001456`, kernel SHA-256
`f694f67e639db1e4465797545c3cca4b00883bd5f068ae2a3bf3dcd08d97c861`,
and stock-host reference revision
`aa60be5f194e0a10e9be2af068166965ba99f276`. The fixed SRAM load/entry and
exact ready-payload gate are essential protocol boundaries. A missing stock
broadcast-suppression step caused preparation rejection; matching the author's
actual host sequence resolved it. Security traffic is intentionally omitted here.
After completed RAM-kernel execution, key OFF/ON alone did not restore normal
ECM diagnostics. A documented full power cycle did, and stock identity/CVNs
matched afterward. That recovery from RAM execution is **not recovery from
partially erased flash**. A failure before RAM execution does not automatically
justify the same restoration action.

The reader and bounded loggers gate on fresh traffic, exact identity, explicit
allowlists, deadlines and cleanup. Preparation stays offline; an actual live
operation is a separate, target-specific decision. The observed battery-only
session and unusable voltage PID do not establish a general power specification.

## Image structure and consistency

**D04 — Segment extraction.** The installed calibration spans `020000–07FFFF`
(393,216 bytes), comprising System, Fuel, Speedometer, Engine Diagnostics and
Engine Operation. The OS spans `080000–2FFFFF`; a separate boot region exists.
Directory pointers, segment identifiers and CRC/CVN agreement support these
ranges. The earlier shortcut that the first 512 KiB is all bootloader is wrong
for this image. Extraction is a raw address-relative slice, not a proprietary
programming container.

**D05 — Seventeen checksums.** The pinned UniversalPatcher E78 layout at revision
`de421df36e195c3afc45e589aea0210afdf6beff` identifies seven CRC16 records,
seven segment wordsums and three additional engine wordsums. All 17 match the
original. The CRC is reflected polynomial `0xA001`, initial zero; the published
audit also checks its standard test vector. Inner engine wordsums are covered
by outer checks. Seventy-three deliberate in-memory mutations were detected.
This validates the recorded checksum model, not an assertion that the ECU has
no other acceptance criteria.

**D06 — Corrected candidates and block plans.** Offline correction computes
nested checks in dependency order, rejects protected metadata/out-of-scope edits,
creates fresh private outputs and verifies all 17 checks. A no-op correction is
byte-identical to the original. A separate core plans complete physical
calibration blocks 4–7, requiring terminal erase completion, aligned transfers,
sequence/retry consistency, final transfer completion, full readback and block
inventory. Ten memory-peer tests and a native harness using upstream
UDS/LZ4/SHA code exercised simulated programming. The native harness uses
simulated flash callbacks; it does not execute ECU flash hardware. No live
flash launcher or physically validated interrupted-write recovery follows.

## Definition research and control-path discoveries

**D07 — References are hypotheses until checked.** A community original/modified
Astra pair helped identify shapes and possible names. Matching OS headers did
not imply matching executable content: its OS region differed from this image
at 1,797,533 bytes and its OS checks failed. Modified donor changes included
protection/diagnostic regions and incomplete checksum consistency. Donor edits
are not a tune template. Other-software XDF offsets/scalings must not be copied.
The source image, callers, load instructions, axes and active configuration
establish each own-image definition.

**D08 — Startup bases and configuration.** Own startup sets r13=`40008000`,
r14=`40017000`, r2=`00088210`. Borrowing another project's r14 would shift RAM
addresses by `0x1000`. Own pedal conversion-enable is disabled, so its zero ratio
is unused rather than evidence of division by zero. Selected mode and branch
flags determine which apparent table interpretation applies.

**D09 — Ten static tables, 1,473 cells.** Eight primary tables use row-major,
big-endian float32 cells with endpoint clamping and linear/bilinear lookups.
Two integer modifiers have independently traced encoding. These are research
definitions for this exact file, not a complete ECU description.

| Definition | Data address | Shape | Selected output |
| --- | --- | --- | --- |
| Maximum boost-control pressure limit | `04DFB4` | 1 × 7 | kPa absolute |
| Indicated peak engine torque | `0534A4` | 4 × 16 | Nm |
| Maximum brake torque limit | `0535A4` | 4 × 16 | Nm |
| Overboost indicated torque limit | `053464` | 1 × 16 | Nm |
| Pedal progression A/B/C | `046944 / 046DC8 / 04724C` | 17 × 17 each | kW on selected mode-1 path |
| Pedal fallback | `0476D0` | 17 × 17 | kW on selected mode-1 path |
| Pedal barometric multiplier | `046664` | 17 × 5 | unsigned16 BE /32768 |
| Pedal/RPM modifier | `04675C` | 9 × 9 | signed16 BE /32 |

Primary pedal row is percent, selected column km/h. A/B/C are byte-identical;
fallback is distinct. Peak/brake/overboost torque roles are not interchangeable.
The normal initialized torque row is statically zero-selected; this is not a
measurement of fuel composition. Boost-limit units are absolute pressure, not
wastegate duty or a promised manifold boost target; its temperature input's
physical location and loaded behavior remain unverified.

**D10 — Inputs and selections.** Own CAN dispatch links request `1F1` and report
`3D1` to A/B/C selection, subject to retained-state gates and fallback. Both saved
drives requested/reported A throughout; no B/C or fallback transitions were
observed. Own pedal producer supports percent rather than the raw sensor PID
percentages. Selected speed derives from `0F9`; the normal prediction
`N × 15 / 494` km/h agreed closely with `3E9` in 5,501 and 6,000 paired samples.
This is internal CAN consistency, not independent road-speed calibration or
proof that suspected wheel-sensor data are healthy.

**D11 — Power, modifiers and arbitration.** The selected pedal path converts
conditioned power through `K = 30000 / (pi × max(filtered_RPM, 300))`.
Runtime affine drivetrain conversion follows; a startup multiplier of 24 is not
a fixed operating ratio. The barometric multiplier is unity throughout this
image. The other modifier uses the own-code selected RPM column and gate;
its reference name does not establish a different speed threshold. Transition
math, baseline subtraction, conditioning and limiting prevent a single raw
pedal interpolation from reproducing the complete request. Scheduling periods
must be traced before per-update changes are called per-second rates.

## Runtime access and observed limits

**D12 — Broadcast/diagnostic consistency.** Engine-off captures correlate the
learned CAN pedal with two normalized sensor PIDs while preserving their offsets.
RPM/speed were zero; coolant matched; small IAT/barometric differences remained
recorded. Coarse internal reads supported one peak-torque bin and one capped
pressure point. These same-system checks do not calibrate sensors independently,
validate a pressure ceiling under load or establish healthy running fueling.

**D13 — Downstream diagnostic requests.** Own callbacks establish `2434` as
internal axle request, `247C` as converted torque increment and `2810` as
processed pedal. All were accepted in a bounded vehicle batch. Endpoint codes
saturate and cannot be treated as ordinary numerical bins or physical engine
output. The saved engine-off captures contained no `1C5` comparison frames;
transmission enabling was not attempted.

**D14 — Fallback flag and stock ordinary RAM.** A complete traced callback links
`1121` mask `0x20` to current fallback control. A separate traced stock service23
path allows selected small ordinary-RAM reads without a security unlock,
session change or RAM kernel. An actual fixed 21-request batch read 56 bytes,
matching a bracketing diagnostic request and observing conversion/cache/
complement states. Zero-point math agreed. Only the exact tested allowlist is
established; restricted/security ranges remain excluded. The earlier rejected
flash-address-zero read did not disprove ordinary RAM access.

**D15 — Raw pedal lookup comparison.** A first capture stopped before diagnostics
because fresh traffic was absent. A separately requested retry completed 164
requests and 14 groups. Four groups met predefined stability criteria and matched
the raw lookup: three released and one nonzero hold near 63.84% pedal, zero speed,
40.686485 kW internal request. Broader predefined coverage was **inconclusive**;
whole-map validation remains false. Sequential sampling and timing qualifications
matter. Pedal tests for this stage ended; these results do not justify repeating
the same sweep to close unrelated gaps.

**D16 — A localized offline editing example.** One experimental candidate changes
only pedal-A's 62% row across 17 columns to the stock interpolated 63% equivalent.
Seventeen checks pass; 55 bytes differ including checksum storage, solely in
physical block 6. A 13,634-point offline interpolation check found the change
confined to the selected band. That is file/arithmetic evidence, not measured
performance or proof of suitable values. A now differs from B/C and transition
implications remain unresolved. No candidate binary is published here and
`flash_ready:false` remains appropriate.

## Overrun, spark and simulation

**D17 — DFCO structures.** Own code identifies state-selected delay maps,
qualification offsets and a torque-ramp multiplier. Selected raw values and
callers are recorded, but timer scheduling/units, full selector enumeration,
physical axes, final fuel-cut output and exit/idle behavior are unfinished.
A donor seconds-per-count annotation is not an own-image timer definition.
“Dual pulse” in the reference describes split fuel injection; it is not a
crackle command.

**D18 — Minimum-spark path.** Own callers identify a 17 × 33 signed16 main map,
a fallback vector, a temperature-related offset and downstream clamps. Actual
conversion instructions multiply the signed result by exactly 1/128. The result
is a minimum-limit path; changing it alone does not command that ignition angle.
Full overrun request selection, later spark arbitration, injector fuel-cut and
thermal behavior are not reconstructed. No overrun/spark edit or validated
crackle calibration exists.

**D19 — Isolated firmware emulation.** Ghidra executed the unmodified lookup with
synthetic registers/stack for 100 input pairs: 64 grid points matched stored
values exactly, and 36 other cases stayed inside the predeclared two-count
rounding bound. Eight signed conversion cases matched raw/128 bit-for-bit.
ROM stayed unchanged. This is isolated instruction execution, not a booted ECU,
peripheral/combustion simulation or whole spark-task validation. The e500/SPE
language selection is a decoding aid, not identification of the ECU as e500.

**D20 — Tool capability boundaries.** The downloaded GM-ECU-Simulator was
source-inspected, not run; its protocol simulation and simple engine character
do not execute this firmware. Earlier completed programming tests used separate
memory/native harnesses. SavvyCAN is a capture/decoder tool, GMLanDebug's serial
path is not RH02 SLCAN, Flashy's Feather firmware is not RH02 firmware, and
EFIGenie is a custom-application/bootloader-patch research track. None supplies
a proven complete stock-calibration write/recovery solution for this vehicle.

## Rejected shortcuts worth retaining

**D21 — Negative evidence prevents repeated mistakes.** Empty early captures
were resolved by physical wiring correction, not a USB reset. A USB version
check needed close/settle/version ordering, not a firmware replacement. Donor
addresses, generic DBC layouts, low-immediate matches and adjacent instructions
were leads that sometimes resolved to different functions or address bases.
Short frames and speed sentinels were not silently decoded as valid physical
values. Diagnostic 204D is constant zero, not the nearby fallback getter.
A unique constant spark pattern does not establish identity or activation.
Passing host tests, multiple agents agreeing, or a successful RAM-read restoration
do not establish vehicle flash safety or tune quality.

A fault snapshot and suspected wheel-sensor issue remain unresolved; no codes
were cleared or suppressed as part of this research. Diagnosis was explicitly
deferred. Recorded advertised sensor availability is not proof that every
measurement needed for loaded calibration is available or trustworthy.

## What another project can reproduce

The minimum honest path is: identify its own target; acquire and preserve two
matching originals with known coverage; reproduce its exact software structure
and checksums; verify definitions against callers/configuration; make one
explicit offline proposal; and independently review its diff, checksums and block
plan. Public synthetic fixtures can demonstrate arithmetic and file handling
without implying target compatibility. A different original must fail exact-image
guards until an independently reviewed profile exists.

A physical tune additionally needs a suitable baseline and measurements, a
supported writer for that target, tool-supported power, validated recovery and
controlled loaded validation. Those dependencies are not supplied by a checksum
pass or this handbook. The current project ends at reviewed research artifacts;
its open boundaries are part of the result, not omitted steps.
