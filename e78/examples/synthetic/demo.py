#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Public synthetic example: invented bytes, no firmware or hardware required."""
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from synthetic_demo import run

if __name__ == "__main__":
    print(json.dumps(run(), indent=2))
