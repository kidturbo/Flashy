# Minimal tools and files

There are two different deliverables: producing an internally consistent candidate and completing a physically validated tune. This release supplies the first for one exact baseline. The second still needs a target-supported writer/recovery path and vehicle measurements; there is no honest “one file flash” recipe here.

## Smallest offline kit

Python 3 with only its standard library is enough for worksheet export, candidate build, checksums and block planning. Tested interpreter versions are recorded in RELEASE-REPORT.md. No CAN driver, Ghidra, GUI editor, .NET, compiler or SDR software is needed for these commands.

| Required file | Purpose |
| --- | --- |
| `tools/e78_pedal_edit.py` | Export all 289 pedal-A cells and build an explicitly proposed CSV edit |
| `tools/e78_candidate.py` | Exact-baseline checks, protected scope, dependency-ordered checksum correction |
| `tools/audit_e78_checksums.py` | Discover and verify the 17 recorded checksum fields |
| `tools/extract_verified_calibration.py` | Exact layout/identity constants and extraction helpers |
| `tools/e78_programming.py` | Full physical-block diff plan; contains offline-tested injected-transport research state machine |
| `tools/e78_read_protocol.py` | Pinned kernel constants and decoder imported by the planner |
| `vendor/SegmentSeek-e78.xml` | Pinned checksum-address signatures with SHA-256 guard |
| Your private exact original | 3 MiB baseline; never downloaded from this project or committed |
| Your CSV proposal + reason per changed cell | Explicit requested edits; start from exported baseline |

Keep LICENSE and vendor notices when sharing the code. This is the minimal dependency closure, not a reason to omit the handbook, tests or source evidence. The compressed decoder's optional `lz4` dependency is not used by the worksheet/plan workflow. The package has no live transport.

For an exact matching baseline, use a fresh private output directory on every run. Supply its location through `E78_PRIVATE_BASELINE` or the default `private/original.bin`; see vendor/README.md and tools/README.md for exact commands. A different image is rejected even if its OS string matches.

## Files produced privately

Export produces the original-valued CSV and baseline metadata. Build produces a 3 MiB derived candidate, copied requested-edits CSV, checksum/cell manifest, full physical-block plan, and SHA256SUMS. Binary output is read-only; directories are exclusive/private. All remain under an ignored private path. Keep review and off-host backups outside public Git too.

## Additional capabilities for an actual project

| Stage | Additional tools/files | Current availability and limit |
| --- | --- | --- |
| Identity and backup | Target-verified reader, correct adapter/wiring, private identity, kernel/source hashes if applicable, read/coverage manifests, two matching originals, off-host copy | Worked RH02 read was vehicle-verified; this minimal package does not ship its live host integration |
| Different software definition | Own original, disassembler such as Ghidra, validated language model, source references, complete table/axis/caller evidence, independent checks | Requires new exact profile and tests; no automatic family-wide definition generator |
| Runtime correlation | Bounded logger with exact fixed allowlist; independently defined sensor channels, units and time alignment | Worked stock diagnostic/RAM evidence is summarized; current allowlist is not a general memory scanner |
| Actual writing | Exact compatible writer/tool container, supported stable power and host, validated target recovery, operation authorization | This project's physical write/recovery remains unvalidated; no live launcher supplied |
| Loaded tune validation | Healthy baseline, relevant verified channels/instruments, controlled environment, logging operator, abort criteria and repeatable comparison | No completed performance/crackle validation is claimed |

Optional research repositories are pinned in docs/sources.md. They are not all required installations. A protocol simulator is not a firmware emulator; isolated lookup emulation is not a complete engine simulation. Keep the build host distinct from any ECU/adapter deployment target.
