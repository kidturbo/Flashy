---
name: Bug Report
about: Report a firmware bug, flash failure, or communication error
title: '[BUG] '
labels: bug
assignees: ''
---

### Module and Firmware

- **Module type:** (E38 / E67 / T87 / T87A / E92 / Other)
- **Firmware version:** (paste output of `STATUS` command)
- **Kernel type:** (extracted from commercial tool / clean-room Kernel / N/A)

### Hardware

- [ ] Adafruit Feather M4 CAN Express
- [ ] AdaLogger FeatherWing (SD + RTC)
- **OBD-II cable:** (brand/type, or bench wiring)

### What happened

A clear description of the bug or failure.

### Expected behavior

What should have happened instead.

### Serial log

Paste the full serial monitor output. If using the Python tools, include the complete console output.

```
PASTE LOG HERE
```

### Steps to reproduce

1. Connect to ...
2. Send command `...`
3. ...

### Binary file info (if applicable)

- **.bin file size:**
- **Source:** (read from ECU / modified / downloaded)
- **Checksum:** (from `t87a_patch.py --verify` or read output, if available)

### Additional context

Add any other context — SavvyCAN captures, screenshots, vehicle year/model, etc.
