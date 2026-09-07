"""Boundary coverage for the hardware callback-headroom report."""

import pathlib
import sys
import unittest

sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts")
)  # noqa: E501
from callback_performance import Decision  # noqa: E402


class DecisionTest(unittest.TestCase):
    def test_documented_bands_keep_their_names_and_gate_behavior(self):
        cases = (
            (59.99, True, "COMFORTABLE", True),
            (60.0, True, "STAY", True),
            (69.99, True, "STAY", True),
            (70.0, True, "REVIEW", False),
            (79.99, True, "REVIEW", False),
            (80.0, True, "UPGRADE", False),
            (80.0, False, "HOLD", False),
        )
        for percent, remaining, name, passes in cases:
            with self.subTest(percent=percent, remaining=remaining):
                decision = Decision.classify(percent, remaining)
                self.assertEqual(decision.name, name)
                self.assertEqual(decision.passes, passes)
                self.assertEqual(decision.blocked_reason is None, passes)


if __name__ == "__main__":
    unittest.main()
