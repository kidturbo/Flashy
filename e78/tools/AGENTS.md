# Offline tools and tests

Inherit ../AGENTS.md. Preserve the exact-image digest, signature digest, fixed calibration scope, protected metadata, finite-value checks, explicit cell reasons, dependency-ordered checksums, exclusive output creation and flash_ready=false. File-internal consistency is not ECU acceptance or calibration safety. Do not turn input-range checks into claims of safe numerical tune limits.

The six-module core is the minimal dependency closure; the table exporter is optional. Keep worksheet/candidate/plan commands standard-library only. Network, serial/CAN, device setup and live unlock/loader/reset launchers are out of scope. The separately included read state machine is exercised only through injected synthetic transport during development. The injected programming state machine remains a separately labeled offline research component; its CLI creates a plan only.

Use PYTHONPATH=tools python3 -m unittest discover -s tests -v from the repository root. Private integration tests require the exact externally supplied baseline and must skip explicitly when it is absent. Never commit a fixture copied from firmware or bypass guards for public CI. Keep synthetic checks independent and document what they establish. Output and data remain under ignored private/ or external private storage.

For adaptations, record originating source hashes and exact modifications in vendor provenance. Separate already established definitions from claims that a portable export re-ran the private traces. Run the release validator and update the explicit manifest after changes. Share file ownership before parallel edits and have the coordinator integrate/review.
