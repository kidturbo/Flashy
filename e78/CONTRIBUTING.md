# Contributing

Read AGENTS.md and the scoped instructions before changing code or evidence. Submit one coherent change with exact profile applicability, source provenance, actual offline test results and unresolved limits. Use synthetic fixtures for public tests; never attach firmware, VIN/identity reports, raw captures or security traffic to issues or pull requests.

New profiles require the evidence checklist in docs/adapting.md and independent frozen-artifact review. Definition names from another OS are leads. Do not remove hash/range/metadata checks to make an unsupported input work. Do not add a live launcher under the claim that simulated protocol tests validate hardware.

Use templates/change.md and templates/definition-review.md where applicable. Preserve GPL and third-party notices. Run the public test suite and scripts/validate_release.py. Private integration checks must be reported as private, with software/hash scope and measured results; public CI cannot reproduce them without the owner's private input.

The coordinator owns release manifests, evidence reconciliation and publication. Keep live operation requests separate from documentation/code review.
