# Sources and provenance

This release is authored from retained local research dated2026-11-09 through2026-11-12 UTC. The [evidence ledger](evidence-ledger.json) pins the summarized private document versions. Private provenance labels are not publicly downloadable artifacts. Public code provenance is in [vendor/provenance.json](../vendor/provenance.json); release-manifest.json pins the shipped files.

| Source | Pinned reference | Role |
| --- | --- | --- |
| UniversalPatcher | [de421df36e195c3afc45e589aea0210afdf6beff](https://github.com/joukoy/UniversalPatcher/tree/de421df36e195c3afc45e589aea0210afdf6beff) | Segment directories, checksum algorithms and signature interpretation; GPL-derived Python audit/corrector |
| E78 signatures | [SegmentSeek-e78.xml](https://github.com/joukoy/UniversalPatcher/blob/de421df36e195c3afc45e589aea0210afdf6beff/XML/SegmentSeek-e78.xml) | Pinned LF-normalized snapshot shipped with license |
| E78 segment definition | [e78.xml](https://github.com/joukoy/UniversalPatcher/blob/de421df36e195c3afc45e589aea0210afdf6beff/XML/e78.xml) | Source interpretation reflected in extraction constants |
| KernelMPC5566 | [320adc3a18985aff5614f2cd2ce5fc40ac001456](https://github.com/FL0WL0W/KernelMPC5566/tree/320adc3a18985aff5614f2cd2ce5fc40ac001456) | Version1.1 kernel protocol; fixed addresses/hash; no kernel binary/source checkout bundled |
| Stock host reference | [aa60be5f194e0a10e9be2af068166965ba99f276](https://github.com/FL0WL0W/ESP32-ECU-Flasher/tree/aa60be5f194e0a10e9be2af068166965ba99f276) | Separate stock upload sequence; not a universal unlock/loader |
| E78_14T | [e254204369505000b63030826f45a90f7398523e](https://github.com/sbarisic/E78_14T/tree/e254204369505000b63030826f45a90f7398523e) | Other-software reference names/patterns only; donor offsets not copied as own-image definitions |
| HP Tuners E78 guide | [E78 torque/power workflow](https://www.hptuners.eu/help/vcm_editor_tuning_how_to_advanced_e78.htm) | Conceptual context already retained in research; not validation of our addresses or numerical draft |

SavvyCAN, Flashy, EFIGenieE78 and GM-ECU-Simulator were assessed as distinct capabilities, not prerequisites for this minimal kit. Their source/licensing belongs to their upstream projects; no third-party checkout is republished here. Physical CAN FD, alternate adapters, generic A2L/XDF compatibility and complete firmware simulation were not established.

The bundled tool sources originated in the private research workspace; the public port preserves exact-image acceptance criteria. See [integration provenance](../vendor/integration-provenance.json) for the added decoder, metadata reconstruction, A/B/C port and formatting. GPL-3.0-or-later applies to this combined distribution. Preserve original notices and source attribution; no claim of ownership over OEM firmware is made, and no such firmware is supplied.
