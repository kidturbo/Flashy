# Publication and maintenance plan

## Initial release scope

Publish the smallest self-contained offline toolkit and an authored handbook. Include synthetic examples, private-input tests that transparently skip without the exact baseline, scoped AGENTS.md files, exact upstream provenance and a checked release manifest. Do not publish the originating collection, its full history, compiled verbatim documentation, or any vehicle data.

## Release process

1. Freeze the staged public tree. Run synthetic/public tests; report private exact-profile tests separately.
2. Review file ownership/provenance, GPL notices, all relative links, exact-image rejection and no-device boundaries. Scan the allowlist for VIN-like strings, credentials, private paths, serials and unintended binary/capture artifacts; automated patterns are a check, not proof that all sensitive data is absent.
3. Record actual environment/results in RELEASE-REPORT.md. Run `python3 scripts/validate_release.py --write` to update the explicit release-manifest.json, then run without --write to verify every hash/link. Unexpected files fail validation rather than being silently added.
4. Initialize/commit only this directory. Inspect staged file names; do not git-add the parent collection. Publish to the selected GitHub/GitLab repository and verify visibility, file list and remote commit. Record the actual URL and commit privately; do not claim a planned upload happened.
5. Run public CI from a clean checkout. Private-input suites are intentionally skipped on public CI. A successful CI job establishes public package consistency, not vehicle support.

GitHub is the proposed initial host; the Git repository itself is portable to GitLab. The GitHub workflow can be mirrored with equivalent Python test/validator commands on GitLab. No hosting-specific service is needed to use the toolkit locally.

## Proposed milestones

| Milestone | Deliverable | Acceptance |
| --- | --- | --- |
|0.1 research/candidate kit | This source-only release | Public tests, private port checks, independent release audit, manifest, verified remote upload |
|0.2 portable profile architecture | Separate validated profiles, schema and rejection tests | At least one independently established second baseline; no digest-only bypass |
|0.3 optional reader/logging tier | Sanitized explicit-port tooling, fixed allowlists, dependencies and recovery-state documentation | Offline protocol/PTY checks plus separately scoped target validation; no automatic live CI |
|Programming capability | Target-supported write/readback and recovery evidence | Independent bench/target-specific evidence, reviewed operation, supported power and authorization |
|Validated calibration example | Reproducible baseline/changed measurement package | Relevant healthy baseline, actual controlled loaded measurements, protection/transition review |

Milestones are proposals, not completed validations. Do not use this vehicle for destructive interruption experiments by implication. No extra diagnostic/pedal tests are queued.

## Agent collaboration

Root AGENTS.md defines evidence/privacy/operation boundaries and standing offline delegation. docs/AGENTS.md owns public evidence; tools/AGENTS.md owns guards and offline verification; definitions/AGENTS.md owns software applicability; examples/AGENTS.md owns synthetic and worked-example integrity. One coordinator integrates/reviews and publishes. A separate conversation or project task is not automatically a child agent; shared files still require coordinated ownership.

## Flashy host-tool integration

The current root is `e78/` inside Flashy. Its explicit release manifest remains
limited to this component; parent firmware/files are not scanned or relabeled.
The parent CI workflow runs these offline tests from `e78/`. Read the current
[scope](flashy-integration.md) and [release results](../RELEASE-REPORT.md) rather
than treating the original0.1 milestones as the latest artifact inventory.
