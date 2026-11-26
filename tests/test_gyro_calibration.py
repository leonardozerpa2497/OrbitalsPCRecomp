"""Exercise the runtime's real calibration storage inside the launcher's real profile, session and save contract."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from launcher import core, reports  # noqa: E402

CALIBRATION = "gyro_calibration.txt"


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
  const float raw[3] = {0.002f, -0.003f, 0.005f};
  if (!strcmp(argv[2], "clock-reset")) {
    still_sample(0, &p, 36000000000000ull, raw, 0);
    for (int i=0; i<=80; i++) still_sample(0, &p, 106000000000ull + i*25000000ull, raw, 0);
    return p.calibrated == 1 && fabsf(p.bias[1] + 0.003f) < 0.000001f ? 0 : 4;
  }
  if (!strcmp(argv[2], "clock-jump")) {
    /* 0.24 s of SDL sensor events (device clock, 250 Hz), then the events stop and polling feeds the host clock every 16 ms */
    for (int i=0; i<60; i++) still_sample(0, &p, 106000000000ull + i*4000000ull, raw, 0);
    u64 host = 36000000000000ull;
    for (int i=0; i<5; i++) still_sample(0, &p, host + i*16000000ull, raw, 0);
    if (p.calibrated) return 5;
    for (int i=5; i<=100; i++) still_sample(0, &p, host + i*16000000ull, raw, 0);
    return p.calibrated == 1 ? 0 : 6;
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
        self.root = Path(self.temp.name).resolve()
        game = self.root / "game"
        for name in ("exefs", "romfs"):
            (game / core.DATA / name).mkdir(parents=True)
        for name in (core.EXE, core.MODULE):
            (game / name).write_bytes(b"binary")
        self.game = core.Installation(game, game / core.DATA / "exefs", game / core.DATA / "romfs", {}, None)
        self.profiles = core.Profiles(self.root / "home")
        self.write_save(self.profiles.saves, b"first")
        self.runs = []

    def tearDown(self):
        for run in self.runs:
            if run.exists():
                core.prune_run_payload(run)
        self.temp.cleanup()

    @staticmethod
    def write_save(saves, body):
        (saves / "user").mkdir(parents=True, exist_ok=True)
        (saves / "user" / "ProfileSave").write_bytes(b"GVAS" + body)

    def session(self):
        """A fresh session laid out by the launcher itself: Orbitals_data/save is its link to the active profile's saves."""
        with mock.patch.object(core, "shader_tools", lambda game: None):
            run = core.session_directory(self.game, self.profiles)
        self.runs.append(run)
        return run / core.DATA

    def call(self, data, operation, *bias, pid="0df2"):
        return subprocess.run([str(self.exe), str(data), operation, "054c", pid,
                               *map(str, bias)], capture_output=True, text=True)

    def load(self, data, pid="0df2"):
        result = self.call(data, "load", pid=pid)
        return result.stdout.strip() if result.returncode == 0 else result.returncode

    def finish(self, data):
        core.prune_run_payload(data.parent)
        shutil.rmtree(data.parent)

    def test_new_session_reads_same_profile_but_other_profile_is_isolated(self):
        first = self.session()
        self.assertEqual(self.call(first, "store", .001, -.002, .003).returncode, 0)
        self.finish(first)
        self.assertEqual(self.load(self.session()), "0.001000 -0.002000 0.003000")
        self.profiles.create("Other")
        self.assertEqual(self.load(self.session()), 3)

    def test_recalibration_preserves_other_controller_models(self):
        first, second = self.session(), self.session()
        self.assertEqual(self.call(first, "store", .001, .002, .003).returncode, 0)
        self.assertEqual(self.call(first, "store", .004, .005, .006, pid="0ce6").returncode, 0)
        self.assertEqual(self.call(second, "store", -.001, -.002, -.003).returncode, 0)
        self.assertEqual(self.load(second), "-0.001000 -0.002000 -0.003000")
        self.assertEqual(self.load(second, pid="0ce6"), "0.004000 0.005000 0.006000")

    def test_failed_write_keeps_previous_calibration(self):
        first, second = self.session(), self.session()
        self.assertEqual(self.call(first, "store", .001, .002, .003).returncode, 0)
        (self.profiles.directory / (CALIBRATION + ".tmp")).mkdir()
        self.assertEqual(self.call(second, "store", .004, .005, .006).returncode, 2)
        self.assertEqual(self.load(second), "0.001000 0.002000 0.003000")

    def test_saves_stay_exportable_and_restore_keeps_calibration(self):
        running = self.session()
        self.assertEqual(self.call(running, "store", .001, .002, .003).returncode, 0)
        self.assertEqual(list(core.save_files(self.profiles.saves)), [Path("user/ProfileSave")])
        self.assertEqual(list(reports.report_saves(self.profiles.saves)), [Path("user/ProfileSave")])
        self.assertTrue((self.profiles.directory / CALIBRATION).is_file())
        exported = self.profiles.export(self.root)
        backup = self.profiles.backup()
        self.assertFalse((exported / CALIBRATION).exists())
        other = self.root / "other-save"
        self.write_save(other, b"second")
        self.profiles.restore(other)
        self.assertEqual((self.profiles.saves / "user" / "ProfileSave").read_bytes(), b"GVASsecond")
        self.assertEqual(self.load(running), "0.001000 0.002000 0.003000")
        self.assertEqual(self.load(self.session()), "0.001000 0.002000 0.003000")
        self.profiles.restore(backup)
        self.assertEqual((self.profiles.saves / "user" / "ProfileSave").read_bytes(), b"GVASfirst")
        self.assertEqual(self.load(self.session()), "0.001000 0.002000 0.003000")
        self.assertEqual(list(core.save_files(self.profiles.saves)), [Path("user/ProfileSave")])

    def test_standalone_install_stores_beside_its_save_directory(self):
        data = self.root / "standalone" / core.DATA
        (data / "save").mkdir(parents=True)
        self.assertEqual(self.call(data, "store", .001, .002, .003).returncode, 0)
        self.assertTrue((data / CALIBRATION).is_file())
        self.assertEqual(list((data / "save").iterdir()), [])
        self.assertEqual(self.load(data), "0.001000 0.002000 0.003000")

    def test_unresolved_or_unexpected_save_link_stores_nothing(self):
        odd = self.root / "odd" / "saves"
        odd.mkdir(parents=True)
        data = self.root / "odd-run" / core.DATA
        data.mkdir(parents=True)
        core.link_directory(data / "save", odd)
        self.assertEqual(self.call(data, "store", .001, .002, .003).returncode, 2)
        self.assertEqual(self.load(data), 3)
        gone = self.root / "gone" / "save"
        gone.mkdir(parents=True)
        dangling = self.root / "dangling-run" / core.DATA
        dangling.mkdir(parents=True)
        core.link_directory(dangling / "save", gone)
        gone.rmdir()
        self.assertEqual(self.call(dangling, "store", .001, .002, .003).returncode, 2)
        self.assertEqual(sorted(p.name for p in self.root.rglob(CALIBRATION + "*")), [])
        for link in (data / "save", dangling / "save"):
            link.rmdir() if core.WINDOWS else link.unlink()

    def test_sensor_clock_switch_restarts_still_calibration(self):
        self.assertEqual(self.call(self.session(), "clock-reset").returncode, 0)

    def test_forward_clock_jump_restarts_still_calibration(self):
        self.assertEqual(self.call(self.session(), "clock-jump").returncode, 0)


if __name__ == "__main__":
    unittest.main()
