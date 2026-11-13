# What remains before a completed tune

A tune is a reviewed calibration operating as intended on the target vehicle, with measured behavior and a viable recovery path. A valid checksum and an edited pedal curve establish neither engine output nor safety.

The current case has unresolved faults and incomplete loaded measurements. Diagnosis was deferred; that does not establish a healthy baseline or justify numerical increases. No validated boost, fueling, spark, torque, octane-specific or crackle prescription is supplied.

| Gate | Evidence needed | Current project status |
| --- | --- | --- |
| Target and baseline | Actual engine/transmission/ECU identity, trusted backup and known configuration | Exact installed baseline retained; factory-stock provenance/hardware service numbers not fully established |
| Definitions | Complete relevant map units, axes, state selection, transitions, modifiers and protection relationships | Ten static maps plus new non-editable spark/overrun structures; full control chain incomplete |
| Measurements | Validated channels, sensor ranges, units, timing, measured vs estimated distinction | Several engine-off correlations and fixed reads; running/loaded validation incomplete |
| Candidate | Explicit justified values, complete diff, all checks, independent frozen-artifact review | One localized experimental pedal-A revision reviewed offline only |
| Physical writer | Exact compatible tool/protocol/container and full readback verification | Simulated protocol tests only; no validated open live writer in this package |
| Recovery and power | Target-specific recovery after actual interrupted/corrupted write, tool-supported stable power and host continuity | Unvalidated; successful post-read power-cycle restoration is a different capability |
| Acceptance | Controlled repeatable baseline/changed comparisons and abort criteria | Not completed |

For any future controlled measurement plan, record fuel/configuration/ambient and engine conditions, verified channels/instruments, time alignment, operating region, operator duties, abort criteria and expected observable result. Distinguish commanded lambda from measured lambda, spark advance from knock correction, internal torque requests from measured output, and absolute pressure from gauge pressure. Avoid inventing thresholds from unrelated examples.

Unexpected knock, lean operation, pressure loss, overheating, misfire or shifting problems require stopping the test and diagnosis. Preserve knock/thermal/overboost/fuel/torque protections and diagnostic reporting. Never use model falsification or fault suppression to hide a problem.

The spark discovery is a minimum permitted advance path, not an overrun spark command. DFCO delay state selectors and timer units remain incomplete. Dual pulse means split fueling in the reference; a constant negative-spark pattern is not an activation recipe. No crackle candidate follows from those leads. Isolated lookup emulation cannot model engine, catalyst or exhaust thermal behavior.

Use supported target-specific tool guidance for any future write and recovery; a generic full-write escalation is not justified. No live write is authorized by this handbook or by producing a candidate.
