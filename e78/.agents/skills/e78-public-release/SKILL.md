---
name: e78-public-release
description: Prepare and verify this E78 Tuning repository's source-only public release. Use for packaging or publishing this repository, not vehicle reading, calibration selection or live programming.
---

Work from the repository root containing release-manifest.json. Read AGENTS.md and docs/publication.md. The release input is scripts/validate_release.py's explicit PUBLIC_FILES allowlist, never the parent research collection. An unexpected file is a review decision; do not automatically extend the allowlist or copy the parent's compiled documentation.

Run the synthetic demo and `PYTHONPATH=tools PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v`. Public CI skips private integration cases explicitly. If the exact private baseline is independently available, E78_PRIVATE_BASELINE can point outside this checkout; never copy it into release inputs. Preserve profile/signature guards and distinguish actual private results from public reproducibility.

Record actual environment, results and unresolved publication/validation limits in RELEASE-REPORT.md. Inspect intended content, privacy/provenance and local links; only then run `python3 scripts/validate_release.py --write` and rerun without --write. The validator checks hashes, explicit files, binary/symlink exclusion and scoped privacy patterns. It does not establish all privacy facts, validate external links or prove vehicle compatibility.

Commit/package only the explicit manifest files plus the manifest. For a source archive, exclude .git, private inputs, test caches and every parent file. Inspect archive inventory against that same allowlist. Publication needs a specified repository and working connection; check existing authorization and stop the external step if access is absent while completing local preparation. Do not switch an existing repository's visibility or overwrite unrelated history by implication. Verify the actual remote commit/file inventory before reporting published; otherwise report the concrete missing connection/destination.

These checks were verified on Python3.14.8/Linux ARM64 for the initial release candidate. Live ECU operations, firmware distribution and claims of physical writing/recovery or tune validation remain outside this skill.
