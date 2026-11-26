"""Exercise the runtime's real calibration storage across launcher-style session directories."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GyroCalibrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="orbitals-calibration-build-")
        cls.exe = Path(cls.build.name) / "calibration"
        source = Path(cls.build.name) / "calibration.c"
        source.write_text(r'''
#include "orbitals_input.c"
#include <sys/stat.h>
char g_datadir[1024];
int plat_mkdir(const char *p) {
#ifdef _WIN32
  return CreateDirectoryA(p, NULL) ? 0 : -1;
#else
  return mkdir(p, 0755);
#endif
}
void logf_(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void ini_set(const char *key, const char *value) { (void)key; (void)value; }
int main(int argc, char **argv) {
  if (argc != 5 && argc != 8) return 10;
  snprintf(g_datadir, sizeof g_datadir, "%s/", argv[1]);
  GyroPad p = {0}; p.vendor = strtoul(argv[3], 0, 16); p.product = strtoul(argv[4], 0, 16);
  if (!strcmp(argv[2], "clock-reset")) {
    const float raw[3] = {0.002f, -0.003f, 0.005f};
    still_sample(0, &p, 36000000000000ull, raw, 0);
    for (int i=0; i<=80; i++) still_sample(0, &p, 106000000000ull + i*25000000ull, raw, 0);
    return p.calibrated == 1 && fabsf(p.bias[1] + 0.003f) < 0.000001f ? 0 : 4;
  }
  if (!strcmp(argv[2], "store")) {
    if (argc != 8) return 11;
    for (int i=0; i<3; i++) p.bias[i] = strtof(argv[5+i], 0);
    return calib_store(&p) ? 0 : 2;
  }
  if (!calib_load(&p)) return 3;
  printf("%.6f %.6f %.6f\n", p.bias[0], p.bias[1], p.bias[2]);
  return 0;
}
''')
        build = subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-ffunction-sections", "-fdata-sections",
                        "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
                        "-I", str(ROOT / "runtime"), str(source), "-Wl,--gc-sections",
                        "-lm", "-o", str(cls.exe)], capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError(build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="orbitals-calibration-test-")
        self.root = Path(self.temp.name)
        self.save = self.root / "profile" / "save"
        self.save.mkdir(parents=True)
        self.first = self.session("first", self.save)
        self.second = self.session("second", self.save)

    def tearDown(self):
        self.temp.cleanup()

    def session(self, name, save):
        data = self.root / name / "Orbitals_data"
        data.mkdir(parents=True)
        (data / "save").symlink_to(save, target_is_directory=True)
        return data

    def call(self, data, operation, *bias, pid="0df2"):
        return subprocess.run([str(self.exe), str(data), operation, "054c", pid,
                               *map(str, bias)], capture_output=True, text=True)

    def test_new_session_reads_same_profile_but_other_profile_is_isolated(self):
        self.assertEqual(self.call(self.first, "store", .001, -.002, .003).returncode, 0)
        shutil.rmtree(self.first.parent)
        result = self.call(self.second, "load")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "0.001000 -0.002000 0.003000")
        other_save = self.root / "other-profile" / "save"
        other_save.mkdir(parents=True)
        other = self.session("other", other_save)
        self.assertEqual(self.call(other, "load").returncode, 3)

    def test_recalibration_preserves_other_controller_models(self):
        self.assertEqual(self.call(self.first, "store", .001, .002, .003).returncode, 0)
        self.assertEqual(self.call(self.first, "store", .004, .005, .006, pid="0ce6").returncode, 0)
        self.assertEqual(self.call(self.second, "store", -.001, -.002, -.003).returncode, 0)
        self.assertEqual(self.call(self.second, "load").stdout.strip(), "-0.001000 -0.002000 -0.003000")
        self.assertEqual(self.call(self.second, "load", pid="0ce6").stdout.strip(), "0.004000 0.005000 0.006000")

    def test_failed_write_keeps_previous_calibration(self):
        self.assertEqual(self.call(self.first, "store", .001, .002, .003).returncode, 0)
        (self.save / "gyro_calibration.txt.tmp").mkdir()
        self.assertEqual(self.call(self.second, "store", .004, .005, .006).returncode, 2)
        self.assertEqual(self.call(self.second, "load").stdout.strip(), "0.001000 0.002000 0.003000")

    def test_sensor_clock_switch_restarts_still_calibration(self):
        self.assertEqual(self.call(self.first, "clock-reset").returncode, 0)


if __name__ == "__main__":
    unittest.main()
