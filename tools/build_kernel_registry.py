#!/usr/bin/env python3
"""build_kernel_registry.py — scan Cernels/ for kernels + meta.json, emit
auto-generated C headers that the firmware includes to build the runtime
kernel registry.

Produces three artifacts (all gitignored):
  src/kernels_generated/<id>.h     — one per kernel: byte array + metadata macros
  src/kernels_public.h             — #includes all public entries + registers them
  src/kernels_private.h            — #includes all private entries (gitignored)

Scans:
  Cernels/<target>_*/kernel.bin + meta.json     (public — committed)
  Cernels/private/*/kernel.bin + meta.json      (private — user's local library)

Used as a PlatformIO pre-build hook:
  in platformio.ini:  extra_scripts = pre:tools/build_kernel_registry.py

Also runnable standalone for testing.

Design principles:
  - Fail SOFT: if anything goes wrong, print warning and emit empty registry
    headers so the existing #include-guarded static kernels keep working.
  - Vendor-neutral: never hard-code tool names. User's meta.json display_name
    is the only free-form string; it passes through verbatim.
  - Folder name == kernel ID. Lowercase + [a-z0-9_] only. Non-conforming
    folders are silently skipped with a warning.
"""
from __future__ import annotations
import json
import os
import re
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# PlatformIO entrypoint is wired at the BOTTOM of the file, after run() is
# defined. See the end of this file for the `Import("env")` block.


# ---------------------------------------------------------------------------
# Core implementation

ID_RE = re.compile(r"^[a-z0-9_]+$")


_PROJECT_DIR_OVERRIDE: Path | None = None


def _find_project_dir() -> Path:
    if _PROJECT_DIR_OVERRIDE is not None:
        return _PROJECT_DIR_OVERRIDE
    # Standalone invocation: __file__ is available. SCons sets our CWD to
    # the project root already, so cwd is a good fallback.
    try:
        here = Path(__file__).resolve().parent
        return here.parent
    except NameError:
        return Path.cwd()


def _sanitize_c_ident(name: str) -> str:
    out = []
    for ch in name:
        if ch.isalnum() or ch == "_":
            out.append(ch)
        else:
            out.append("_")
    if out and out[0].isdigit():
        out = ["_"] + out
    return "".join(out).lower() or "_unnamed"


def _parse_hex(val):
    """Accept 0xNN or NN or int."""
    if isinstance(val, int):
        return val
    if isinstance(val, str):
        s = val.strip().lower()
        if s.startswith("0x"):
            return int(s, 16)
        if s.startswith("$"):
            return int(s[1:], 16)
        return int(s, 0)
    raise ValueError(f"Cannot parse hex: {val!r}")


def _read_meta(meta_path: Path) -> dict:
    """Read meta.json, strip _comment_* keys, validate minimum fields."""
    with open(meta_path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    # Strip comment keys
    clean = {k: v for k, v in raw.items() if not k.startswith("_")}
    for section in ("upload_format", "handoff", "probe", "expected_reply"):
        if isinstance(clean.get(section), dict):
            clean[section] = {
                k: v for k, v in clean[section].items() if not k.startswith("_")
            }
            for sub_key, sub_val in list(clean[section].items()):
                if isinstance(sub_val, dict):
                    clean[section][sub_key] = {
                        k: v for k, v in sub_val.items() if not k.startswith("_")
                    }
    # Validate minimum
    missing = []
    if "target" not in clean:
        missing.append("target")
    if "load_addr" not in clean:
        missing.append("load_addr")
    if missing:
        raise ValueError(f"meta.json missing required fields: {missing}")
    return clean


def _discover_public(root: Path):
    """Find committed kernels: Cernels/*_read/ (or similar) with meta.json + kernel.bin."""
    found = []
    cernels = root / "Cernels"
    if not cernels.is_dir():
        return found
    for sub in sorted(cernels.iterdir()):
        if not sub.is_dir():
            continue
        if sub.name in ("private", "extracted"):
            continue
        meta_path = sub / "meta.json"
        bin_path = sub / "kernel.bin"
        if not meta_path.is_file() or not bin_path.is_file():
            continue
        try:
            meta = _read_meta(meta_path)
            found.append(("public", sub.name, bin_path, meta))
        except Exception as exc:  # noqa: BLE001
            sys.stderr.write(
                f"[kernel_registry] skipping public/{sub.name}: {exc}\n"
            )
    return found


def _discover_private(root: Path):
    """Find user-local kernels: Cernels/private/<anything>/kernel.bin + meta.json."""
    found = []
    private_dir = root / "Cernels" / "private"
    if not private_dir.is_dir():
        return found
    for sub in sorted(private_dir.iterdir()):
        if not sub.is_dir():
            continue
        if sub.name.startswith(".") or sub.name.endswith(".template"):
            continue
        meta_path = sub / "meta.json"
        bin_path = sub / "kernel.bin"
        if not meta_path.is_file() or not bin_path.is_file():
            continue
        if not ID_RE.match(sub.name):
            sys.stderr.write(
                f"[kernel_registry] skipping private/{sub.name!r}: "
                f"folder name must match [a-z0-9_]+ to use as ID\n"
            )
            continue
        try:
            meta = _read_meta(meta_path)
            found.append(("private", sub.name, bin_path, meta))
        except Exception as exc:  # noqa: BLE001
            sys.stderr.write(
                f"[kernel_registry] skipping private/{sub.name}: {exc}\n"
            )
    return found


def _fmt_cstring(s: str | None) -> str:
    if s is None:
        return "NULL"
    # Escape quotes + backslashes
    escaped = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def _gen_kernel_header(dest: Path, src_bin: Path, kid: str, meta: dict) -> None:
    """Emit src/kernels_generated/<kid>.h — byte array + metadata."""
    c_ident = _sanitize_c_ident(kid)
    data = src_bin.read_bytes()

    load_addr = _parse_hex(meta["load_addr"])
    target = meta["target"]
    display_name = meta.get("display_name") or kid

    up = meta.get("upload_format", {}) or {}
    up34 = (up.get("$34") or {}).get("type", "size3")
    up36 = (up.get("$36") or {}).get("type", "addr4")
    up36_block_seq = (up.get("$36") or {}).get("block_seq", 0x80)

    handoff = meta.get("handoff", {}) or {}
    use_37 = bool(handoff.get("use_$37", False))
    auto_jump = bool(handoff.get("auto_jump", True))

    boot_delay = int(meta.get("boot_delay_ms", 500))

    probe = meta.get("probe", {}) or {}
    probe_svc = probe.get("service")
    probe_pid = probe.get("pid")
    probe_svc_int = _parse_hex(probe_svc) if probe_svc not in (None, "null") else 0
    probe_pid_int = _parse_hex(probe_pid) if probe_pid not in (None, "null") else 0

    expected = meta.get("expected_reply", {}) or {}
    sig = expected.get("sig_bytes") if expected.get("sig_bytes") not in (None, "null") else None

    lines = []
    lines.append(f"/* Auto-generated by tools/build_kernel_registry.py — do not edit. */")
    lines.append(f"/* Kernel ID: {kid}  target: {target}  size: {len(data)} bytes */")
    lines.append(f"#ifndef KERNEL_{c_ident.upper()}_H")
    lines.append(f"#define KERNEL_{c_ident.upper()}_H")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append(f"#define KERNEL_{c_ident.upper()}_ID             \"{kid}\"")
    lines.append(f"#define KERNEL_{c_ident.upper()}_TARGET         \"{target}\"")
    lines.append(f"#define KERNEL_{c_ident.upper()}_DISPLAY        {_fmt_cstring(display_name)}")
    lines.append(f"#define KERNEL_{c_ident.upper()}_SIZE           {len(data)}")
    lines.append(f"#define KERNEL_{c_ident.upper()}_LOAD_ADDR      0x{load_addr:08X}u")
    lines.append(f"#define KERNEL_{c_ident.upper()}_UPLOAD_RD     \"{up34}\"   /* $34 RequestDownload fmt */")
    lines.append(f"#define KERNEL_{c_ident.upper()}_UPLOAD_TD     \"{up36}\"   /* $36 TransferData fmt */")
    lines.append(f"#define KERNEL_{c_ident.upper()}_BLOCK_SEQ      0x{up36_block_seq:02X}u")
    lines.append(f"#define KERNEL_{c_ident.upper()}_USE_TXE        {1 if use_37 else 0}  /* send $37 TransferExit */")
    lines.append(f"#define KERNEL_{c_ident.upper()}_AUTO_JUMP      {1 if auto_jump else 0}")
    lines.append(f"#define KERNEL_{c_ident.upper()}_BOOT_DELAY_MS  {boot_delay}")
    lines.append(f"#define KERNEL_{c_ident.upper()}_PROBE_SVC      0x{probe_svc_int:02X}u")
    lines.append(f"#define KERNEL_{c_ident.upper()}_PROBE_PID      0x{probe_pid_int:02X}u")
    lines.append(f"#define KERNEL_{c_ident.upper()}_EXPECTED_SIG   {_fmt_cstring(sig)}")
    lines.append("")
    lines.append(f"static const uint8_t KERNEL_{c_ident.upper()}_BLOB[] = {{")
    for i in range(0, len(data), 16):
        row = ", ".join(f"0x{b:02X}" for b in data[i:i+16])
        lines.append(f"    {row},")
    lines.append("};")
    lines.append("")
    lines.append(f"#endif /* KERNEL_{c_ident.upper()}_H */")
    lines.append("")
    dest.write_text("\n".join(lines), encoding="utf-8")


def _gen_registry_header(
    dest: Path, entries: list, header_name: str, guard: str
) -> None:
    """Emit a src/kernels_{public,private}.h that expands an X-macro list."""
    lines = []
    lines.append(f"/* Auto-generated by tools/build_kernel_registry.py — do not edit. */")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    for (_src, kid, _bin, _meta) in entries:
        c_ident = _sanitize_c_ident(kid)
        lines.append(f"#include \"kernels_generated/{c_ident}.h\"")
    lines.append("")
    lines.append(f"/* X-macro list: KERNEL_{header_name.upper()}_LIST(X) expands X(id_upper) */")
    lines.append(f"#define KERNEL_{header_name.upper()}_LIST(X) \\")
    for (_src, kid, _bin, _meta) in entries:
        c_ident = _sanitize_c_ident(kid)
        lines.append(f"    X({c_ident.upper()}) \\")
    lines.append("    /* end */")
    lines.append("")
    lines.append(f"#endif /* {guard} */")
    lines.append("")
    dest.write_text("\n".join(lines), encoding="utf-8")


def _emit_empty(project_dir: Path) -> None:
    """Fallback for when scanning fails: minimal empty headers."""
    gen = project_dir / "src" / "kernels_generated"
    gen.mkdir(parents=True, exist_ok=True)
    _gen_registry_header(
        project_dir / "src" / "kernels_public.h",
        [],
        "public",
        "KERNELS_PUBLIC_H",
    )
    _gen_registry_header(
        project_dir / "src" / "kernels_private.h",
        [],
        "private",
        "KERNELS_PRIVATE_H",
    )


def run() -> None:
    project_dir = _find_project_dir()
    gen_dir = project_dir / "src" / "kernels_generated"
    gen_dir.mkdir(parents=True, exist_ok=True)

    public = _discover_public(project_dir)
    private = _discover_private(project_dir)

    # Emit one .h per discovered kernel
    seen_ids = set()
    for (src_kind, kid, bin_path, meta) in public + private:
        if kid in seen_ids:
            sys.stderr.write(
                f"[kernel_registry] WARN: duplicate kernel id '{kid}' — "
                f"last wins\n"
            )
        seen_ids.add(kid)
        c_ident = _sanitize_c_ident(kid)
        _gen_kernel_header(gen_dir / f"{c_ident}.h", bin_path, kid, meta)

    # Emit registry X-macro headers
    _gen_registry_header(
        project_dir / "src" / "kernels_public.h",
        public,
        "public",
        "KERNELS_PUBLIC_H",
    )
    _gen_registry_header(
        project_dir / "src" / "kernels_private.h",
        private,
        "private",
        "KERNELS_PRIVATE_H",
    )

    sys.stdout.write(
        f"[kernel_registry] public={len(public)} private={len(private)}  "
        f"total={len(public) + len(private)}\n"
    )
    for (src_kind, kid, bin_path, meta) in public + private:
        sz = bin_path.stat().st_size
        sys.stdout.write(
            f"[kernel_registry]   {src_kind:7s}  {kid:24s}  {meta.get('target'):6s}  {sz} B\n"
        )


if __name__ == "__main__":
    run()


# ---------------------------------------------------------------------------
# PlatformIO SCons entrypoint — invoked at pre-build.

try:
    Import("env")  # type: ignore[name-defined]  # SCons builtin
    _env = env  # type: ignore[name-defined]
    try:
        _PROJECT_DIR_OVERRIDE = Path(_env["PROJECT_DIR"])
    except Exception:  # noqa: BLE001
        _PROJECT_DIR_OVERRIDE = Path.cwd()
    try:
        run()
    except Exception as _exc:  # noqa: BLE001
        sys.stderr.write(f"[kernel_registry] FATAL: {_exc}\n")
        sys.stderr.write(
            "[kernel_registry] Emitting empty registry; build continues.\n"
        )
        try:
            _emit_empty(_find_project_dir())
        except Exception:  # noqa: BLE001
            pass
except NameError:
    pass  # Not under SCons — standalone invocation above handled it.
