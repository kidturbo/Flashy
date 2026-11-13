# Worked pedal-response proposal

This is an offline worked case, not a recommended tune. It uses the exact installed profile in definitions/README.md. No original, full table export or candidate binary is published.

The experimental revision changed only pedal progression A's 62% row, across 17 speed columns, to the original curve's equivalent at 63% pedal:

```text
proposed[column] = float32(original_at_62[column]
                          + (original_at_74[column] - original_at_62[column]) / 12)
```

The one-percentage-point shift was a selected draft parameter, not a number established by fuel octane, hardware limits or an observed performance shortfall. Neighboring rows stay unchanged, so interpolation blends the edit from zero at50% to its maximum at62% and back to zero at74%.

At40km/h, the stored 62% cell changed from75.800003kW to78.408333kW, an internal requested-power increase of2.608330kW. This is not measured horsepower or an increase in peak output. A/B/C were originally identical; editing A creates transition differences that still need assessment.

## Reproduce privately for the exact baseline

1. Follow tools/README.md to export all289 original-valued cells into a new ignored private directory.
2. Copy the CSV. Find the62% and74% pedal rows by axis value, not an assumed row number. Apply the expression to proposed_kw in the62% row only and provide an explicit experimental rationale for each of17 edits.
3. Build a new private output, then review the original/candidate hashes, exact17 changed cells, all17 checksum checks, protected regions and full block plan. Do not submit the private outputs to Git.

The historical revision differed at55 bytes total:48 table-data bytes and7 checksum bytes, solely in calibration block6 (0x040000–0x05FFFF). Recorded offline sampling examined13,634 interpolation points. These figures describe the historical exact proposal; they are not promised for arbitrary edits. If a reproduction differs, stop and examine float32 rounding, inputs and code/source hashes rather than relaxing the comparison.

Before editing, vehicle capture had matched three released samples and one nonzero stationary hold around63.84%/40.686485kW. Broader coverage was inconclusive. The revised file has never been validated in the ECU or under load. Keep flash_ready=false.
