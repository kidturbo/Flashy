# Pinned checksum signatures and attribution

`SegmentSeek-e78.xml` is the existing UTF-8/LF snapshot of
[UniversalPatcher XML/SegmentSeek-e78.xml at de421df36e195c3afc45e589aea0210afdf6beff](https://github.com/joukoy/UniversalPatcher/blob/de421df36e195c3afc45e589aea0210afdf6beff/XML/SegmentSeek-e78.xml).
Its SHA-256 is
`7e6a179d371d54e36a355fdf2b7449d26ff83c80b0d475b053a7970ab64bd4b9`.
The snapshot's GPL version 3 license is retained verbatim as
`UniversalPatcher-LICENSE`; see `provenance.json` for source hashes.

The extraction layout and Python audit/corrector retain their original
UniversalPatcher attribution. The offline read-protocol and programming core
refer to FL0WL0W/KernelMPC5566 Version1.1, commit
`320adc3a18985aff5614f2cd2ce5fc40ac001456`, and the stock host reference
FL0WL0W/ESP32-ECU-Flasher commit `aa60be5`. No upstream kernel binary, stock
firmware, donor image, private capture, or third-party checkout is bundled.
The injected programming core is offline-tested research code; it supplies no
hardware transport or live flashing launcher.

Local tool source paths and hashes before the path portability change are
recorded in `provenance.json`. Only `e78_candidate.py` changes those defaults:
the private baseline defaults to `private/original.bin`, can be selected with
`E78_PRIVATE_BASELINE`, and signatures are resolved in `vendor/`.
All image identity, region, metadata and checksum guards remain unchanged.

## Flashy integration

[Integration provenance](integration-provenance.json) records portable read-state-machine and A/B/C builder adaptations, the metadata-only catalog, and formatting. The stock-loader protocol lineage retains the [ESP32-ECU-Flasher GPL notice](ESP32-ECU-Flasher-LICENSE). No kernel binary is bundled.
