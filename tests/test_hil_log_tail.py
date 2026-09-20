"""Console capture must follow logger restarts as well as file rotation."""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(
    0,
    str(pathlib.Path(__file__).resolve().parent / "hil"),
)
from wavex_target import LogTail  # noqa: E402


class LogTailTest(unittest.TestCase):
    def test_truncated_logger_file_returns_new_ack(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "console.log"
            path.write_bytes(b"previous session\n" * 100)
            tail = LogTail(path)
            try:
                path.write_bytes(b"WAVEX-DBG: 123 OK\n")
                self.assertEqual(
                    list(tail.lines(0.06)),
                    [b"WAVEX-DBG: 123 OK"],
                )
            finally:
                tail.close()

    def test_rotated_logger_file_returns_new_ack(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "console.log"
            path.write_bytes(b"old\n")
            tail = LogTail(path)
            try:
                path.rename(path.with_suffix(".old"))
                path.write_bytes(b"WAVEX-DBG: 124 OK\n")
                self.assertEqual(
                    list(tail.lines(0.06)),
                    [b"WAVEX-DBG: 124 OK"],
                )
            finally:
                tail.close()


if __name__ == "__main__":
    unittest.main()
