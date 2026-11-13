#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate the explicit source-only public release; never traverse private data."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
PUBLIC_FILES = frozenset(
    [
        ".agents/skills/e78-public-release/SKILL.md",
        ".github/workflows/offline.yml",
        ".gitignore",
        "AGENTS.md",
        "CONTRIBUTING.md",
        "LICENSE",
        "README.md",
        "RELEASE-REPORT.md",
        "TOOLS-NOT-SHIPPED.md",
        "definitions/AGENTS.md",
        "definitions/README.md",
        "definitions/research-catalog.json",
        "docs/AGENTS.md",
        "docs/acquisition.md",
        "docs/adapting.md",
        "docs/discoveries.md",
        "docs/evidence-ledger.json",
        "docs/flashy-integration.md",
        "docs/minimum-kit.md",
        "docs/publication.md",
        "docs/replication.md",
        "docs/research-progress.md",
        "docs/sources.md",
        "docs/tuning-and-validation.md",
        "examples/AGENTS.md",
        "examples/pedal-abc-profile.json",
        "examples/pedal-response.md",
        "examples/synthetic/README.md",
        "examples/synthetic/demo.py",
        "pyproject.toml",
        "requirements-dev.txt",
        "requirements.txt",
        "scripts/validate_release.py",
        "templates/change.md",
        "templates/definition-review.md",
        "templates/project.md",
        "templates/review.md",
        "templates/session.md",
        "tests/test_definition_export_offline.py",
        "tests/test_e78_candidate_offline.py",
        "tests/test_e78_programming_offline.py",
        "tests/test_e78_read_protocol_offline.py",
        "tests/test_e78_read_workflow_offline.py",
        "tests/test_pedal_abc_offline.py",
        "tests/test_pedal_edit_offline.py",
        "tests/test_pedal_profile_offline.py",
        "tests/test_public_offline.py",
        "tests/test_research_catalog_offline.py",
        "tools/AGENTS.md",
        "tools/README.md",
        "tools/audit_e78_checksums.py",
        "tools/e78_candidate.py",
        "tools/e78_identity.py",
        "tools/e78_pedal_abc.py",
        "tools/e78_pedal_edit.py",
        "tools/e78_programming.py",
        "tools/e78_read_protocol.py",
        "tools/e78_read_workflow.py",
        "tools/export_research_catalog.py",
        "tools/export_table_definitions.py",
        "tools/extract_verified_calibration.py",
        "tools/synthetic_demo.py",
        "vendor/ESP32-ECU-Flasher-LICENSE",
        "vendor/README.md",
        "vendor/SegmentSeek-e78.xml",
        "vendor/UniversalPatcher-LICENSE",
        "vendor/integration-provenance.json",
        "vendor/provenance.json",
        "vendor/source-provenance.json",
    ]
)
MANIFEST = "release-manifest.json"
IGNORED_DIRS = frozenset((".git", "__pycache__", ".venv", "private"))
SENSITIVE = {
    "personal absolute path": re.compile(r"/(?:home|root|Users)/[A-Za-z0-9_.-]+/"),
    "VIN-like identifier": re.compile(
        r"(?<![A-Za-z0-9_])[1-5][A-HJ-NPR-Z0-9]{16}(?![A-Za-z0-9_])"
    ),
    "credential token": re.compile(
        r"(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|-----BEGIN (?:RSA |OPENSSH |EC )?PRIVATE KEY-----)"
    ),
}


def collect(root):
    paths = set()

    def visit(directory):
        for path in directory.iterdir():
            rel = path.relative_to(root).as_posix()
            if path.is_symlink():
                raise ValueError(f"Symlink not allowed in public tree: {rel}")
            if path.is_dir():
                if path.name not in IGNORED_DIRS:
                    visit(path)
            elif path.is_file():
                paths.add(rel)

    visit(root)
    return paths


def inspect(root):
    found = collect(root)
    unknown = found - PUBLIC_FILES - {MANIFEST}
    missing = PUBLIC_FILES - found
    if unknown or missing:
        raise ValueError(
            f"Explicit allowlist mismatch; unexpected={sorted(unknown)}, missing={sorted(missing)}"
        )
    # If a Git checkout is present, private/ignored content must also be untracked.
    if (root / ".git").exists():
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True
        )
        tracked = set(filter(None, result.stdout.decode().split("\0")))
        if tracked - PUBLIC_FILES - {MANIFEST}:
            raise ValueError("Tracked files exceed public allowlist")
    rows = []
    for name in sorted(PUBLIC_FILES):
        path = root / name
        raw = path.read_bytes()
        if b"\0" in raw:
            raise ValueError(f"Binary content forbidden: {name}")
        text = raw.decode("utf-8")
        for label, pattern in SENSITIVE.items():
            if pattern.search(text):
                raise ValueError(f"{label} detected in {name}; inspect privately")
        if path.suffix == ".json":
            json.loads(text)
        if path.suffix == ".md":
            for target in re.findall(r"(?<!!)\[[^\]]*\]\(([^)]+)\)", text):
                target = target.strip().split(' "')[0].strip("<>")
                if not target or target.startswith("#") or urlsplit(target).scheme:
                    continue
                dest = (path.parent / unquote(target.split("#")[0])).resolve()
                if not dest.is_relative_to(root.resolve()) or not dest.exists():
                    raise ValueError(
                        f"Broken/outside local Markdown link in {name}: {target}"
                    )
        rows.append(
            dict(path=name, bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
        )
    return dict(
        schema="e78-public-release-v1",
        source_only=True,
        firmware_included=False,
        hardware_validation_claimed=False,
        files=rows,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--write",
        action="store_true",
        help="Refresh only the reviewed explicit file allowlist",
    )
    args = parser.parse_args()
    report = inspect(ROOT)
    target = ROOT / MANIFEST
    if args.write:
        target.write_text(json.dumps(report, indent=2) + "\n")
    elif not target.exists() or json.loads(target.read_text()) != report:
        raise ValueError("Release manifest differs; inspect edits before --write")
    print(
        f"Verified {len(report['files'])} public text files, hashes, relative links and scoped privacy patterns."
    )
    print("Private evidence and external links were not validated by this check.")


if __name__ == "__main__":
    main()
