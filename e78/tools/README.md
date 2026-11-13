# Offline tool commands

Run commands from the repository root with Python 3. The six core modules are
`e78_pedal_edit.py`, `e78_candidate.py`, `audit_e78_checksums.py`,
`extract_verified_calibration.py`, `e78_programming.py`, and
`e78_read_protocol.py`. Their worksheet/candidate/plan workflow uses only the
standard library. No command below opens a vehicle adapter.

The default private baseline is `private/original.bin`. To keep the file outside
this checkout, set `E78_PRIVATE_BASELINE` to its private absolute path. It must
match the exact documented image digest; another owner's E78 image is rejected.
Every output path below must be new. Keep outputs under ignored `private/`.

```sh
python3 tools/e78_pedal_edit.py export private/worksheet-01
# Edit only proposed_kw and reason in private/worksheet-01/pedal-a.csv.
python3 tools/e78_pedal_edit.py build private/worksheet-01/pedal-a.csv private/candidate-01
python3 tools/e78_programming.py private/candidate-01/pedal-a-research-candidate.bin private/plan-01.json
```

An unchanged worksheet is refused rather than creating a spurious candidate.
The builder checks all 17 recorded checksums and limits edits to pedal-A data
plus checksum storage. `flash_ready` remains false. The injected programming
state machine has no supplied hardware transport or live launcher.

Extraction and a read-only checksum audit take the private source explicitly:

```sh
python3 tools/extract_verified_calibration.py private/original.bin private/extract-01
python3 tools/audit_e78_checksums.py private/original.bin vendor/SegmentSeek-e78.xml private/audit-01.json
```

## Optional seventh tool: exact-image table definitions

`export_table_definitions.py` exports ten qualified research definitions and
all 1,473 cells from a matching private image into private JSON/CSV files:

```sh
python3 tools/export_table_definitions.py private/definitions-01
# Or select a private baseline explicitly:
python3 tools/export_table_definitions.py private/definitions-02 --source private/original.bin
```

This is optional for the pedal candidate workflow. `build(data)` and its exact
image guard are preserved from the research exporter. The portable export
records the original exporter hash and public discovery-ledger references;
private reference CSVs and Ghidra traces are not shipped or reverified by this
command. Read each definition's units, active configuration and limitations.
Definitions remain non-editable and not ready to flash; they cover only the
documented subset. No original cell values are supplied by this repository.

## Tests without private firmware

```sh
python3 tools/synthetic_demo.py
PYTHONPATH=tools python3 -m unittest discover -s tests -v
```

Private integration classes skip when the baseline is absent. For private
integration verification, supply your exact baseline through
`E78_PRIVATE_BASELINE` and run the same test command. Tests use temporary files
that are cleaned afterward. Synthetic examples demonstrate algorithms and
unknown-image rejection, not ECU simulation or tuning validation.

## Flashy integration additions

The [package README](../README.md) documents the matched A/B/C builder,
108-record research exporter, injected original-read workflow and pure identity
decoder. These extend the historical six-module minimum described above.
The read workflow has no adapter/backend launcher. Its optional tests require
`E78_KERNEL_IMAGE` and `lz4`; missing inputs are explicit skips.
