"""Build and run the gyro gate tests (tests/test_gyro_gate.c) against the runtime's real orbitals_input.c."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GyroGateTests(unittest.TestCase):
    def test_gate(self):
        with tempfile.TemporaryDirectory(prefix="orbitals-gyro-gate-") as build:
            exe = Path(build) / "gyro_gate"
            result = subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-ffunction-sections", "-fdata-sections",
                                     "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-I", str(ROOT / "runtime"),
                                     str(ROOT / "tests" / "test_gyro_gate.c"), "-Wl,--gc-sections", "-lm", "-o", str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
