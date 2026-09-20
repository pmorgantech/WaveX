"""Boundary coverage for the hardware callback-headroom report."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(
    0, str(pathlib.Path(__file__).resolve().parents[1] / "scripts")
)  # noqa: E501
from callback_performance import Decision, parse_capture  # noqa: E402


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


class CaptureTest(unittest.TestCase):
    def test_diagnostic_images_cannot_be_recorded_as_acceptance_evidence(self):
        config = (
            "profile_config: core_hz=480000000 sample_rate=48000 "
            "block_size=48 storage=qspi opt=-O2 link=UART"
        )
        window = "audio_callback: calls=5000 avg_cycles=100 max_cycles=200\n"
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "capture.log"
            markers = (" callback_detail=1\n", "\ncallback_peak: window=1\n")
            for marker in markers:
                path.write_text(config + marker + window)
                with self.subTest(marker=marker):
                    with self.assertRaisesRegex(SystemExit, "diagnostic only"):
                        parse_capture(path)
            for marker in ("\n", " callback_detail=0\n"):
                path.write_text(config + marker + window)
                self.assertEqual(parse_capture(path).max_cycles, 200)


if __name__ == "__main__":
    unittest.main()
