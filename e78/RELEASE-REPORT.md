# Flashy E78 integration verification

Prepared 2026-11-12 on native Kali Linux ARM64 with Python3.14.8. This report
records package checks; it does not establish Feather or vehicle support.
The upstream base is `d714d5c473864a812f84e2e8ff0c21224a802976`.

## Executed checks

- Public suite:64 tests discovered;19 passed and45 explicitly skipped without
  private firmware/the optional published kernel. Compressed protocol tests used
  `lz4==4.4.5`. Without that optional dependency, two further cases skip.
- Private integration run:all64 tests passed in107.836seconds. Both the untouched
  exact baseline and the separately pinned public kernel were supplied through
  external paths; neither was copied into this component.
- The portable A/B/C builder reproduced candidate SHA-256
  `c2dd06b5134798bb836d10b5215bbac6f00acb57ee371ac9d279437cefead53f`.
  Checks cover the changed region, preserved tables, checksums, immutable output,
  rejected mismatches and no-change handling. It remains an experimental draft.
- The current catalog reconstructed108 records/15,893 logical cells, including
  both VCP spark maps and the recorded inline-axis qualification. Wrong image,
  modified image and corrupted record hashes were rejected. The export leaves
  proposal/reason columns blank and refuses an existing destination.
- Read-workflow cases exercised synthetic raw/LZ4 peers, identity rejection,
  communication-control ordering, ready gating, sequence/retry/deadline behavior,
  partial-read preservation, paired hashes and missing terminal acknowledgements.
  These tests do not execute the kernel or access a vehicle.
- Black25.11.0 formats Python source/tests/scripts/examples. It checks AST
  equivalence when formatting. Public tests and the strengthened catalog-export
  checks are rerun after formatting; source lineage is recorded separately from
  final shipped-file hashes.

The synthetic demo, Black check (24 Python files), and explicit release validator
passed. The manifest covers69 public text files plus the manifest itself.
The strengthened catalog tests passed all six cases after formatting. The validator checks the reviewed file allowlist,
text-only content, hashes, JSON, local Markdown links and scoped privacy patterns.
Manual review additionally covers provenance and excluded data. These scans do
not prove every privacy fact or validate external links.

## Reproduction

From `e78/`:

```sh
python3 -m pip install -r requirements-dev.txt
python3 -m black --check tools tests scripts examples
python3 examples/synthetic/demo.py
PYTHONPATH=tools PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v
python3 scripts/validate_release.py
```

Optional inputs are `E78_PRIVATE_BASELINE` and `E78_KERNEL_IMAGE`; exact digest
checks stay enabled. Public CI targets Python3.11 and3.14. The executed local
interpreter is3.14.8; a workflow file alone is not a hosted CI result.

## Contribution boundaries

The source-only GPL component adds no firmware binary, live launcher, firmware
registration or change to Flashy's existing read/write commands. Stock images,
candidates, cell arrays, captures, VINs, security exchanges, serials and emulator
databases remain private. Catalog addresses/types/scales/hashes are included.

Upstream requests Phase A serial evidence for every PR. It has not been run;
no Feather is available. This is a draft contribution for scope/license review,
with the unmet hardware check stated explicitly. No Phase B/C result, physical
flash/recovery validation, complete ECU simulation, loaded tune or power gain is
claimed. Research candidates retain `flash_ready=false`.
