# E78 host research toolkit for Flashy

Portable Python tools for inspecting an exact GM E78 image, defining calibration
records, building reviewable pedal candidates, and testing read/programming
protocol state machines without a vehicle. This is a separate GPL component;
Flashy's Feather firmware and MIT clean-room kernels retain their existing scope.

**Current capability:** source-only research and offline candidate preparation.
The originating project completed two matching RH02 main-flash reads, but this
package supplies no live adapter launcher. Physical Flashy E78 integration,
writing, interrupted-write recovery and a validated performance tune remain open.

## Quick start — no ECU or firmware needed

Requires Python **3.11 or newer**. From the Flashy repository:

```sh
cd e78
python3 examples/synthetic/demo.py
PYTHONPATH=tools python3 -m unittest discover -s tests -v
python3 scripts/validate_release.py
```

The minimal file workflow uses the standard library. Tests requiring the private
baseline or optional published kernel explicitly skip when those inputs are
absent. For formatting and compressed-protocol tests in your own environment:

```sh
python3 -m pip install -r requirements-dev.txt
python3 -m black --check tools tests scripts examples
```

Linux is the first host target. Offline Python components are portable; Android
USB/root/terminal integration is not supplied. No GUI, graphs, gauges or guided
in-car logging app has been released by this contribution.

## Included tools

| Tool | Purpose | Output / boundary |
| --- | --- | --- |
| `extract_verified_calibration.py` | Extract five calibration segments | Fresh private files, exact baseline only |
| `audit_e78_checksums.py` | Discover/check the recorded checksum fields | 17 checks; file consistency only |
| `e78_candidate.py` | Correct checksums in dependency order | Boot/OS/metadata protection, immutable candidate |
| `e78_pedal_edit.py` | Export/edit pedal-A CSV | Explicit proposals/reasons; no-change edits refused |
| `e78_pedal_abc.py` | Build the matched A/B/C profile example | Original low/full pedal and low-speed behavior retained; block6 only |
| `export_table_definitions.py` | Original ten-table export | 1,473 cells from a private matching image |
| `export_research_catalog.py` | Latest 108-record export | 15,893 logical cells; metadata ships without stock cell arrays |
| `e78_read_protocol.py` | Pinned RAM packet planning and upload decoding | Raw/LZ4 framing, sequence, range and final-ack checks |
| `e78_read_workflow.py` | Original-read state machine | Injected transport, identity/ready gates, two matching reads; no live backend |
| `e78_identity.py` | Pure decoding of three identity replies | No scanner, security exchange capture or device access |
| `e78_programming.py` | Physical-block plan and injected programming core | Full-block ordering/retries/readback in offline tests; CLI only makes a plan |

Read [the Flashy integration guide](docs/flashy-integration.md) before adapting
any transport. Flashy's `FLSHY` kernel protocol is different from the pinned
KernelMPC5566 `99`/ISO-TP protocol. The host libraries do not make the Feather or
RH02 interchangeable, and packet planning does not establish restoration.

## Exact-image profile

The worked image is 3 MiB, OS **12644082**, EngineOper **55581437**, SHA-256:

```text
3b9b58421977ae2a5fb0f5d4d1bf71702cd97922109a215cab1ba9405e3920ba
```

Its factory-unaltered provenance is not proved. Same model, ECU family or OS
number does not establish compatibility. Another image is rejected; changing
only a hash constant is not a port. See [adaptation](docs/adapting.md).

Keep your image outside public source, or under ignored `private/`. You can set
`E78_PRIVATE_BASELINE` to an external private path. No original, candidate,
donor image, kernel binary, VIN, adapter serial, raw capture, shadow/password
value or Ghidra database is distributed in this package.

## Offline workflow with your matching private original

Every destination must be fresh. The tools never overwrite the original.

```sh
# Default private input: private/original.bin
python3 tools/extract_verified_calibration.py private/original.bin private/extract-01
python3 tools/audit_e78_checksums.py private/original.bin vendor/SegmentSeek-e78.xml private/audit-01.json

# Manual pedal-A worksheet; edit proposed_kw and reason before building.
python3 tools/e78_pedal_edit.py export private/worksheet-01
python3 tools/e78_pedal_edit.py build private/worksheet-01/pedal-a.csv private/candidate-01

# Separate experimental matched A/B/C example.
python3 tools/e78_pedal_abc.py examples/pedal-abc-profile.json private/abc-01

# Current research definitions, reconstructed privately.
python3 tools/export_research_catalog.py private/definitions-01

# Offline block planning only — this command cannot flash.
python3 tools/e78_programming.py private/abc-01/pedal-abc-response-draft.bin private/plan-01.json
```

The A/B/C example remaps selected mid-pedal requests and tapers the change with
speed. Its chosen bounds are experimental shape constraints, not validated
engine or transmission limits. It changes neither maximum demand nor boost,
spark, fueling or protections. It is **not a completed 92-AKI or crackle tune**.
Read [research progress](docs/research-progress.md) and inspect every diff before
considering any separate programming workflow.

## Tests and simulation

Public tests use invented byte arrays and protocol peers. Optional integration
checks accept the private baseline through `E78_PRIVATE_BASELINE`. Reader
workflow tests additionally require `lz4` and `E78_KERNEL_IMAGE` pointing to the
separately obtained, hash-verified public KernelMPC5566 Version1.1 asset. The
kernel is not executed by the fake peer and is never bundled here. No production
guard is bypassed to make a synthetic image appear compatible.

The formatter and release validator are separate from behavior tests. See
[actual verification results](RELEASE-REPORT.md), [protocol/evidence boundaries](docs/flashy-integration.md)
and [simulator scope](docs/research-progress.md). Passing an offline suite does
not establish ECU acceptance, combustion safety or physical recovery.

## Research and remaining work

The current catalog includes pedal, torque/model, DFCO, base-spark and
cam-related spark records. Base-spark and VCP lookup work passed 3,831 and 3,719
isolated original-instruction cases respectively in the originating research.
Those private runs are summarized here; the emulator databases and stock tables
are not shipped. Cam-related spark tables are not desired cam-position tables.

Remaining work includes cam-target/feedback interpretation, selected boost and
fueling definitions, qualified current loaded measurements, and physical
programming/recovery validation. A checksum-correct candidate does not close
those gaps. All candidates retain `flash_ready=false`.

## Documentation

- [Flashy integration and completion matrix](docs/flashy-integration.md)
- [Current mapping, simulation and calibration status](docs/research-progress.md)
- [Acquisition evidence and restoration limits](docs/acquisition.md)
- [Original detailed discoveries](docs/discoveries.md)
- [Definition profile](definitions/README.md)
- [Replication workflow](docs/replication.md)
- [Tools and source kit](docs/minimum-kit.md)
- [Excluded/private components](TOOLS-NOT-SHIPPED.md)
- [Source provenance](vendor/provenance.json) and [this integration's changes](vendor/integration-provenance.json)
- [Release validation](docs/publication.md) and [contribution guidelines](CONTRIBUTING.md)

The explicit release manifest contains source, tests, public-safe documentation
and metadata only. It is not an archive of the originating private workspace.

## License

This `e78/` component is [GPL-3.0-or-later](LICENSE), with retained upstream
licenses and attribution under `vendor/`. It is not compiled or linked into
Flashy's MIT Feather firmware. In particular, stock-loader protocol lineage is
attributed to FL0WL0W/ESP32-ECU-Flasher, and checksum signatures to UniversalPatcher.
No ownership of OEM firmware is claimed and no such firmware is included.
