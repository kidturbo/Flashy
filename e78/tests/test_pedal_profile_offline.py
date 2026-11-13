# SPDX-License-Identifier: GPL-3.0-or-later
"""Profile rejection checks that need neither firmware nor a device."""
import copy
import json
import unittest

from e78_pedal_abc import PROFILE, validate_profile


class ProfileTests(unittest.TestCase):
    def test_documented_example_and_invalid_numeric_or_endpoint_changes(self):
        profile = json.loads(PROFILE.read_text())
        self.assertEqual(len(validate_profile(profile)), 17)
        for index, value in ((0, 1), (16, 101), (12, float("nan")), (12, 57)):
            changed = copy.deepcopy(profile)
            changed["stock_equivalent_pedal_percent"][index] = value
            with self.assertRaises(ValueError):
                validate_profile(changed)

    def test_profile_cannot_claim_validation_or_change_image_scope(self):
        profile = json.loads(PROFILE.read_text())
        for key, value in (
            ("flash_ready", True),
            ("vehicle_validated", True),
            ("original_sha256", "0" * 64),
            ("tables", ["pedal_a"]),
        ):
            changed = copy.deepcopy(profile)
            changed[key] = value
            with self.assertRaises(ValueError):
                validate_profile(changed)
