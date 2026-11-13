# E78 Tuning agent instructions

## Scope and evidence

This is a public research-to-offline-candidate toolkit. Read README.md, docs/minimum-kit.md, docs/discoveries.md and applicable nested AGENTS.md before implementation. Search relevant persistent memory if available, then inspect existing code/tests/evidence. Memory is an aid, not a prerequisite for a contributor without that service. Reuse established solutions and preserve recorded failures.

Bind claims to a date, source revision/hash, exact ECU/software profile and evidence class: source inspection, offline calculation, emulated instructions, protocol simulation, bench or vehicle. Never describe a planned test as completed. Research definitions, valid checksums and agent agreement do not establish safe tuning or flash readiness. Preserve flash_ready=false and hardware_validated=false wherever present.

## Privacy and ownership

Use allowlisted public files. Never commit original/candidate/donor firmware, VIN or identity reports, hardware serials, raw CAN/security traffic, credentials, host-specific paths, Ghidra databases, or private session records. Synthetic examples must state their origin. Keep a private untouched baseline and verified off-host copy. Do not weaken exact-image guards to accept another owner's image. Retain licenses and provenance. Inspect Git status before edits and do not overwrite another contributor's work.

## Delegation

Standing authorization permits independent offline subagents within the assigned task. The coordinator chooses a useful split, names file owners, gives evidence paths/profile hashes and defines deliverables/tests/stop conditions. Use one agent for small or tightly coupled tasks. Respect the session concurrency limit; child delegation needs coordinator agreement. Inherit model settings unless explicitly directed otherwise.

Suggested roles: evidence researcher, binary/definition analyst, tool implementer, offline test author, and independent frozen-candidate reviewer. Separate writers by file and outputs by directory; serialize dependent changes. Each agent returns findings, sources, changed files, actual commands/results and unresolved limitations. The coordinator reconciles evidence, inspects diffs, verifies integration and owns final documentation/release updates.

## Operation boundaries

This repository authorizes offline preparation only. Subagents must not open serial/CAN devices, run hardware tests, transmit diagnostics, change firmware/host settings, request ignition changes, or perform programming/recovery. A live task requires separate exact-operation authorization and a single coordinator on the physically connected host. Complete preparation, fixed request allowlist, timeouts, abort/cleanup and restoration plan before fresh physical-state confirmation. Never infer RUN from an old session. Follow the operator's ignition/power instructions and communicate OFF immediately at batch completion before analysis.

No flash operation follows automatically from candidate creation. Identity/image compatibility, checksums, reviewed changes, supported writer/power, target-specific recovery and suitable measurement validation are separate gates. Never guess security keys, shadow data, loader framing, recovery straps or safe boost/spark/fuel/torque limits. Preserve protection strategies and diagnostic reporting. Continue independent offline work when a dependent gate is missing.

## Checks and records

Run only explicitly offline tests; a test_ filename alone does not establish safety. Use PYTHONPATH=tools python3 -m unittest discover -s tests -v for this package. Do not initiate unrelated builds. For future compiled device releases, identify deployment CPU/ABI separately from host and evaluate supported optimization with correctness and measured benefit; ordinary debug builds remain debug-friendly.

Record problems, established causes, working code/commands, environment, results and limits. Update public evidence/provenance and private records separately. Save verified repeatable procedures as skills when the contributor's environment supports skills. Run scripts/validate_release.py after changes and regenerate the release manifest before publication. Publish only the inspected allowlist, never a parent workspace or its compiled document bundle.
