# Release Process

Flashy uses manual GitHub Releases with attached artifacts.

## Pre-release checklist

- [ ] All changes committed and pushed to `main`
- [ ] `changelog.html` updated with the new version entry
- [ ] Version define updated in `src/Pass-Thru-Protocol.h` (or via `platformio.ini` build flags)
- [ ] Firmware compiles cleanly: `pio run`
- [ ] Python tools build cleanly: `python tools/build_exe.py`

## Build release artifacts

```bash
# Option A — zip only (always works, no extra tooling)
pio run                              # compile firmware
python tools/build_exe.py --zip      # builds tools, copies firmware,
                                     # writes dist/Flashy-Tool.zip

# Option B — zip + Windows installer .exe (recommended for releases)
python tools/build_exe.py --installer
```

The `--installer` flag invokes [Inno Setup 6](https://jrsoftware.org/isinfo.php)
to build `dist/Flashy-Tool-Setup-X.Y.Z.exe` from `installer/Flashy.iss`.
Inno Setup is a one-time install on the build machine; the resulting
installer .exe is self-contained and needs nothing on the user side.

Both artifacts ship together on the GitHub Release page so users can
choose: power users grab the zip, less-technical users run the installer.

Rename the zip to include the version:

```
dist/Flashy-Tool.zip  -->  Flashy-Tool-vX.Y.Z.zip
```

## Create git tag

```bash
git tag -a vX.Y.Z -m "vX.Y.Z - Brief description"
git push origin vX.Y.Z
```

## Create GitHub Release

1. Go to https://github.com/kidturbo/Flashy/releases/new
2. Choose the tag `vX.Y.Z`
3. Title: `vX.Y.Z — Brief description`
4. Body: copy the relevant section from `changelog.html` or write a summary
5. Attach release artifacts (drag into the assets area):
    - `Flashy-Tool-vX.Y.Z.zip` — portable zip
    - `Flashy-Tool-Setup-X.Y.Z.exe` — Windows installer (if built with `--installer`)
6. Check **"Set as the latest release"**
7. Publish

## Version numbering

| Bump | When |
|------|------|
| **Major** (X) | New ECU family support or breaking protocol changes |
| **Minor** (Y) | New commands, new tools, kernel improvements |
| **Patch** (Z) | Bug fixes, documentation updates |
