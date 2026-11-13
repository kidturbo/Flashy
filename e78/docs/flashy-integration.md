# E78 contribution to Flashy

This package turns completed E78 host-side research into portable source and
reproducible offline checks. It is isolated from Flashy's Feather firmware and
its MIT clean-room `Cernels/` workstream. No hardware upload/read/write command
is added to Flashy's serial menu or kernel registry.

## Included functionality and evidence

| Workstream | Included artifact | Established scope |
| --- | --- | --- |
| Original-read protocol | `e78_read_protocol.py`, `e78_read_workflow.py`, pure identity decoder, synthetic protocol tests | Fixed published kernel, exact-profile identity check, stock preparation, descending RAM packets, exact ready byte, bounded two-pass read and failure records; injected transport only |
| File extraction and checksums | Existing extraction/audit/correction core and pinned UniversalPatcher signatures | Exact baseline, five calibration segments, 17 checksum records and protected metadata |
| Manual pedal editing | CSV pedal-A editor and matched A/B/C profile builder | Documented changed cells, checksum correction, immutable output, block-6-only scope; experimental response shaping |
| Current research definitions | 108 metadata-only records plus private-image exporter | Axes/scalars/models/maps totaling 15,893 logical cells; values reconstructed only from caller-owned exact image |
| Programming core | Injected-transport block planner/programmer | Offline inventory, erase ordering, aligned transfers, retries, terminal acknowledgements and full readback checks; no live launcher |
| Regression tests | Synthetic public tests; explicitly optional private/kernel integration cases | No serial/CAN imports or device access in the released workflow/tests |
| Research handbooks | Acquisition, definitions, latest mapping, simulator scope, adaptation and remaining work | Findings with evidence boundaries, rather than a claim of complete tuning or hardware support |

The originating project physically obtained two matching main-flash reads through
RH02 and restored stock diagnostic identity after a full power cycle. That does
not establish this package on Feather, physical flash writing, interrupted-write
recovery, a complete ECU simulator, or a validated performance/crackle tune.

## Different transports and kernel dialects

| Layer | Flashy firmware / clean-room kernel | This E78 component |
| --- | --- | --- |
| Host-to-adapter | Feather USB serial commands | No supplied hardware backend; library transport injection |
| Running kernel | Flashy's `FLSHY` signature and raw command protocol | Pinned KernelMPC5566 Version1.1, ready payload `99`, ISO-TP application services |
| Read flow | Module-specific Flashy paths | Kernel `35`, sequenced `36/76`, terminal `37/77` |
| Stock preparation | Existing module-specific loader logic | Separate E78 stock dialect; do not treat the kernel's dummy seed as a stock unlock |
| Exit/restoration | Module-specific | Read completion does not restore engine control; restoration remains externally established |

KernelMPC5566's entry is `0x40014000`; Flashy's E92/T87A entry addresses and
Feather command paths are not substitutions. Its synthesized ECC ranges are
`0x3FE0..0x3FFF` and `0x1FFE0..0x1FFFF`, totaling 64 bytes. They differ from
the larger generic E78 skip ranges in Flashy's earlier reference page. Preserve
the actual reader/kernel coverage instead of widening or silently mixing masks.
The 3 MiB main-flash read does not include shadow/password memory.

## Remaining Flashy integration work

1. Establish a target-matched Feather-to-E78 transport and bounded application
   payload contract, then test it on isolated hardware. Existing RH02 results do
   not satisfy this test.
2. Integrate and validate the exact stock preparation/uploader and ready gate.
   The included injected state machine supplies a reviewed starting point, not
   approval to enable another module's serial command.
3. Establish post-read restoration for the intended target. A programming
   session, bootloader return and normal engine control are separate states.
4. Complete physical programming, interrupted-write recovery and post-write
   acceptance evidence before exposing a live writer. Existing offline success
   and upstream support claims do not close those gaps.
5. Keep candidate validity separate from engine validation: actual loaded
   response, channel validity and relevant operating conditions remain necessary.

Flashy's contribution guide requires Phase A serial evidence for every PR and
Phase C for flash behavior changes. This component changes no existing firmware
or live serial behavior, but Phase A has not been run: no Feather is available.
It is therefore offered as a draft for the maintainer to assess the separate
GPL host-tool scope and missing Phase A evidence. No Phase A/B/C result is claimed.

## Distribution and maintenance

`e78/` retains GPL and vendor notices; the rest of Flashy is not relabeled GPL.
No stock firmware, donor binary, tuned image, kernel asset, VIN, adapter serial,
raw capture, shadow value or Ghidra database is included. The public definition
file contains locations/types/scales/hashes, not original cell arrays.

Use `scripts/validate_release.py` for the package's explicit text-file allowlist,
hashes, links and scoped privacy checks. The fork's inherited files remain
upstream history; this contribution adds no new binary payloads. Public CI only
runs the `e78/` tests, never Flashy's hardware-facing `tools/test_*.py` scripts.
See [actual release results](../RELEASE-REPORT.md) for what was executed.
