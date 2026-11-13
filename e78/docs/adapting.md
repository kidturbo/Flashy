# Adapt to another owner's ECU

The current production profile is intentionally narrow. There is no CLI option that makes arbitrary E78 software supported. Do not disable a digest, identifier, signature or range guard merely to progress.

Create a separate profile branch only after recording your own two matching original reads and identities. Preserve the existing profile/tests. A new profile review must establish:

1. Read coverage, image size/base, byte order, ECC substitutions, excluded data and provenance.
2. Segment headers/IDs, directory ranges and all applicable CRC/wordsum algorithms/storage. Verify independent implementations, range endpoints, nested dependency order, no-op, mutations, idempotence and rejection of protected edits.
3. Every relevant table's address/header, dimensions, full axes, encoding/signedness, scaling/units, interpolation/clamping, callers, runtime selection and modifiers. Same symbols or similar values in a donor are insufficient.
4. Correct startup register bases and actual address formation. Check complete lis/displacement pairs and actual callbacks; proximity is not a reference.
5. Fixed runtime channels/ranges with supported semantics and independently bounded validation. Static addresses must not become arbitrary probing.
6. Physical block map and exact target protocol. Preserve bootloader/OS, erase-before-write, alignment, sequence/retry behavior, terminal completion and complete readback. Source/protocol tests alone do not validate hardware writing.
7. Public synthetic fixtures plus private exact-profile integration tests and independent review. Record which tests require proprietary/private data and skip them transparently in public CI.

Record applicability as both an exact baseline hash and software/structure evidence. Profiles that intentionally support multiple baselines need demonstrated structural equivalence, individual identity/provenance and negative tests; do not simply broaden acceptance to an OS label.

Use templates/project.md and templates/definition-review.md. Until this work is complete, the new profile remains unsupported/non-editable. You can still use the public methodology and synthetic tests without modifying production guards.
