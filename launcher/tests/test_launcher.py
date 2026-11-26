# SwitchRecomp - Copyright 2026 MZ Labs

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from zipfile import ZipFile

from launcher import core, reports


def build_fixture(path, build_byte=b"n"):
    data = path / core.DATA
    exefs = data / "exefs"
    paks = data / "romfs" / core.PAK_DIR
    exefs.mkdir(parents=True)
    paks.mkdir(parents=True)
    for name in ("main", "rtld", "sdk"):
        (exefs / name).write_bytes(b"NSO0" + bytes(60) + build_byte * 32)
    (exefs / "main.npdm").write_bytes(b"META")
    for pak in core.PAKS:
        (paks / pak).write_bytes(b"Empty test container")
    (path / core.EXE).write_bytes(b"MZ\0\0" if core.WINDOWS else b"\x7fELF")
    (path / core.EXE).chmod(0o755)
    (path / core.MODULE).write_bytes(b"Fixture, no game code")
    return data


class LocalStateTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.profiles = core.Profiles(self.root / "launcher")
        self.native = self.root / "build with spaces"
        self.data = build_fixture(self.native)

    def test_controller_scan_never_runs_an_older_runtime(self):
        game = core.installation(self.native)
        with patch.object(core, "REPO", self.root), patch.object(core.subprocess, "run") as run:
            with self.assertRaisesRegex(core.LauncherError, "updated Orbitals runtime"):
                core.gamepad_inventory(game)
            run.assert_not_called()

    def test_controller_scan_lists_recognized_and_unmapped_devices(self):
        game = core.installation(self.native)
        with game.exe.open("ab") as image:
            image.write(core.GAMEPAD_INVENTORY.encode("ascii"))
        output = f"{core.GAMEPAD_INVENTORY}\nbackend\tSDL\npad\t1\tTest pad\npad\t0\tUSB joystick\n"
        with patch.object(core.subprocess, "run", return_value=SimpleNamespace(stdout=output, returncode=0)) as run:
            self.assertEqual(core.gamepad_inventory(game), ("SDL", [("Test pad", True), ("USB joystick", False)]))
            self.assertEqual(run.call_args.args[0], [str(game.exe), "--list-gamepads"])

    def test_mouse_settings_inherit_existing_ini_until_overridden(self):
        game = core.installation(self.native)
        args, _ = core.launch_options(game, self.root, self.root / "cache", self.profiles.settings)
        self.assertIn("--keyboard_player=1", args)
        self.assertFalse(any(arg.startswith("--mouse_") for arg in args))
        self.profiles.update_active(settings={"keyboard": 2, "mouse_camera": True, "mouse_sensitivity": 145})
        args, _ = core.launch_options(game, self.root, self.root / "cache", self.profiles.settings)
        self.assertTrue({"--keyboard_player=2", "--mouse_right_stick=1", "--mouse_sensitivity=1.45"}.issubset(args))
        with self.assertRaises(core.LauncherError):
            core.validate_settings(self.profiles.settings | {"mouse_sensitivity": 501})

    def test_in_game_settings_follow_the_profile_that_started_the_session(self):
        first = self.profiles.active
        self.profiles.update_active(settings={"mouse_sensitivity": 145})
        run = self.profiles.directory / "runs" / "session"
        run.mkdir(parents=True)
        (run / "orbitals-pc-settings.ini").write_text(
            "[Settings]\nborderless=1\nwindow_height=720\nvsync=0\nfps=60\nscene_height=900\n"
            "volume=65\nkeyboard=2\nmouse=1\nvibration=0\n", encoding="ascii")
        self.profiles.create("Second player")
        core.finish_session(run, 0, self.profiles)
        self.assertEqual(self.profiles.settings, core.DEFAULT_SETTINGS)
        self.profiles.select(first)
        settings = self.profiles.settings
        self.assertEqual(settings, core.DEFAULT_SETTINGS | {
            "display": "Borderless", "size": "1280x720", "vsync": False, "fps": "60 (experimental)",
            "mode": "1600x900", "gfx_preset": "Custom", "volume": 65, "keyboard": 2, "mouse_camera": True,
            "mouse_sensitivity": 145, "vibration": False,
        })
        game = core.installation(self.native)
        args, env = core.launch_options(game, run, self.root / "cache", settings)
        self.assertIn("--fullscreen=1", args)
        self.assertIn("--keyboard_player=2", args)
        if core.WINDOWS:
            self.assertIn("--vibration=0", args)
        self.assertEqual(env["SWR_AUDIO_VOLUME"], "65")
        self.assertEqual(env["NXRT_VK_PRESENT"], "immediate")

    def test_in_game_mouse_sensitivity_updates_the_next_launch(self):
        run = self.profiles.directory / "runs" / "session"
        run.mkdir(parents=True)
        config = run / "orbitals-pc-settings.ini"
        config.write_text("[Settings]\nmouse_sensitivity=175\n", encoding="ascii")
        core.import_pc_settings(run, self.profiles)
        self.assertEqual(self.profiles.settings["mouse_sensitivity"], 175)
        config.write_text("[Settings]\nmouse_sensitivity=501\n", encoding="ascii")
        core.import_pc_settings(run, self.profiles)
        args, _ = core.launch_options(core.installation(self.native), run, self.root / "cache", self.profiles.settings)
        self.assertIn("--mouse_sensitivity=1.75", args)

    def fake_shader_tools(self, prepare_body):
        tools = self.native / "nxshader"
        tools.mkdir()
        (tools / "nxs_live.py").write_text("", encoding="utf-8")
        (tools / "nxs_kraken.py").write_text(
            "import json, os, sys\nout = sys.argv[sys.argv.index('--out') + 1]\nos.makedirs(out, exist_ok=True)\n"
            "open(os.path.join(out, 'ooz_kraken.dll' if os.name == 'nt' else 'libooz_kraken.so'), 'wb').write(b'x')\n"
            "print(json.dumps({'phase': 'built'}))\n", encoding="utf-8")
        (tools / "nxs_prepare.py").write_text(prepare_body, encoding="utf-8")
        return tools

    def test_pipeline_seed_of_the_installed_build_reaches_the_runtime(self):
        tools = self.fake_shader_tools("")
        game = core.installation(self.native)
        run = self.root / "run"
        run.mkdir()
        _, env = core.launch_options(game, run, self.root / "cache", self.profiles.settings)
        self.assertNotIn("NXRT_VK_PIPELINE_SEED", env)
        (tools / "pipeline_seeds").mkdir()
        seed = tools / "pipeline_seeds" / (game.ids["main"][:32] + ".tsv")
        seed.write_text("C\t0123456789abcdef\n", encoding="utf-8")
        _, env = core.launch_options(game, run, self.root / "cache", self.profiles.settings)
        self.assertEqual(env["NXRT_VK_PIPELINE_SEED"], str(seed))

    def test_shader_preparation_relays_progress_and_records_the_summary(self):
        self.fake_shader_tools(
            "import json, os, sys\nargs = sys.argv\ncache = args[args.index('--cache') + 1]\nos.makedirs(cache, exist_ok=True)\n"
            "assert os.path.isfile(args[args.index('--decoder') + 1])\n"
            "print(json.dumps({'phase': 'translate', 'discovered': 4, 'cached': 1, 'translated': 2, 'failed': 1}))\n"
            "done = {'phase': 'done', 'discovered': 4, 'cached': 1, 'translated': 2, 'failed': 1, 'validated': 3, 'seconds': 60}\n"
            "open(os.path.join(cache, 'PREPARED'), 'w').write(json.dumps(done))\nprint(json.dumps(done))\n")
        game = core.installation(self.native)
        self.assertIsNone(core.shader_preparation(game, self.profiles))
        events = []
        summary = core.prepare_shaders(game, self.profiles, on_progress=events.append)
        self.assertEqual([e["phase"] for e in events], ["decoder", "translate", "done"])
        self.assertEqual(summary["validated"], 3)
        self.assertEqual(core.shader_preparation(game, self.profiles)["discovered"], 4)
        decoder = self.profiles.root / "tools" / "kraken"
        self.assertTrue(any(decoder.iterdir()))

    def test_shader_preparation_error_is_reported(self):
        self.fake_shader_tools("import json\nprint(json.dumps({'phase': 'error', 'error': 'no Unreal IoStore container'}))\nraise SystemExit(1)\n")
        game = core.installation(self.native)
        with self.assertRaisesRegex(core.LauncherError, "no Unreal IoStore container"):
            core.prepare_shaders(game, self.profiles)
        self.assertIsNone(core.shader_preparation(game, self.profiles))

    def test_diagnostics_log_the_3d_resolution_only_when_asked(self):
        game = core.installation(self.native)
        run = self.root / "run"
        run.mkdir()
        with patch.dict(core.os.environ, {"NXRT_VK_LOG_RES": "1"}):
            _, env = core.launch_options(game, run, self.root / "cache", self.profiles.settings)
        self.assertNotIn("NXRT_VK_LOG_RES", env)
        with patch.dict(core.os.environ, {"ORBITALS_DIAGNOSTICS": "1"}):
            _, env = core.launch_options(game, run, self.root / "cache", self.profiles.settings)
        self.assertEqual(env["NXRT_VK_LOG_RES"], "1")

    def test_flight_recorder_is_off_for_players_unless_asked(self):
        game = core.installation(self.native)
        run = self.root / "run"
        run.mkdir()
        _, env = core.launch_options(game, run, self.root / "cache", self.profiles.settings)
        self.assertEqual(env["NXRT_VK_FLIGHT"], "0")
        with patch.dict(core.os.environ, {"NXRT_VK_FLIGHT": "1"}):
            _, env = core.launch_options(game, run, self.root / "cache", self.profiles.settings)
        self.assertEqual(env["NXRT_VK_FLIGHT"], "1")

    def test_shader_translator_finds_headers_through_session_link(self):
        link = self.root / "nxshader"
        core.link_directory(link, core.REPO / "tools/nxshader")
        self.addCleanup(link.rmdir if core.WINDOWS else link.unlink)
        result = subprocess.run([sys.executable, "-I", str(link / "nxs_live.py"), "--help"],
                                cwd=self.root, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--cache", result.stdout)

    def test_prepares_matching_data_and_reports_progress(self):
        dumps = self.root / "source"
        shutil.copytree(self.data, dumps)
        (dumps / "control").mkdir()
        (dumps / "control/control.nacp").write_bytes(bytes(0x3060) + b"7.2.1\0")
        progress = []
        dest = core.prepare(dumps, core.installation(self.native), self.root / "installs",
                            lambda done, total: progress.append((done, total)))
        self.assertEqual(dest.ids, core.installation(self.native).ids)
        self.assertEqual(dest.version, "7.2.1")
        self.assertEqual(progress[-1][0], progress[-1][1])
        self.assertTrue((dumps / "exefs/main").is_file())

    def test_missing_files_and_inaccessible_source(self):
        (self.data / "romfs" / core.PAK_DIR / core.PAKS[0]).unlink()
        with self.assertRaisesRegex(core.LauncherError, "Incomplete RomFS"):
            core.installation(self.native)
        self.assertEqual(core.installation(self.native, template=True).root, self.native)
        with self.assertRaises(FileNotFoundError):
            core.prepare(self.root / "absent", core.installation(self.native, template=True),
                         self.root / "installs", lambda *_: None)
        with patch("launcher.core.tree_files", side_effect=PermissionError("Permission denied")):
            with self.assertRaises(PermissionError):
                core.prepare(self.data, core.installation(self.native, template=True),
                             self.root / "installs", lambda *_: None)

    def test_rejects_mismatched_exefs_and_romfs(self):
        template = core.installation(self.native)
        alternative = build_fixture(self.root / "alt", build_byte=b"x")
        target = self.root / "installs"
        with self.assertRaisesRegex(core.LauncherError, "ExeFS version does not match"):
            core.prepare(alternative, template, target, lambda *_: None)
        (alternative / "exefs/main").write_bytes((self.data / "exefs/main").read_bytes())
        (alternative / "exefs/sdk").write_bytes((self.data / "exefs/sdk").read_bytes())
        (alternative / "exefs/rtld").write_bytes((self.data / "exefs/rtld").read_bytes())
        (alternative / "romfs" / core.PAK_DIR / "global.utoc").write_bytes(b"Different table")
        with self.assertRaisesRegex(core.LauncherError, "RomFS version does not match"):
            core.prepare(alternative, template, target, lambda *_: None)

    def test_disk_space_check_and_interrupted_prepare(self):
        template = core.installation(self.native)
        target = self.root / "installs"
        with patch("launcher.core.shutil.disk_usage", return_value=SimpleNamespace(free=0)):
            with self.assertRaisesRegex(core.LauncherError, "Not enough disk space"):
                core.prepare(self.data, template, target, lambda *_: None)
        completed = core.prepare(self.data, template, target, lambda *_: None)

        def interrupted(*_):
            raise InterruptedError

        with self.assertRaises(InterruptedError):
            core.prepare(self.data, template, target, interrupted)
        self.assertEqual(list(target.iterdir()), [completed.root])

    def test_preparation_and_import_reject_links(self):
        folder = self.root / "outside"
        folder.mkdir()
        evil = self.data / "romfs" / "link"
        core.link_directory(evil, folder)
        self.addCleanup(evil.rmdir if core.WINDOWS else evil.unlink)
        with self.assertRaisesRegex(core.LauncherError, "junction"):
            core.prepare(self.data, core.installation(self.native), self.root / "installs", lambda *_: None)
        copied = self.root / "fake-save"
        copied.mkdir()
        core.link_directory(copied / "user", folder)
        with self.assertRaisesRegex(core.LauncherError, "junction"):
            self.profiles.restore(copied)

    def test_settings_are_profile_specific_and_validate_arguments(self):
        main = self.profiles.active
        self.profiles.update_active(settings={"volume": 25, "display": "Borderless"})
        self.profiles.create("Passager")
        self.profiles.rename("Nova")
        self.assertEqual(self.profiles.settings["volume"], 100)
        with self.assertRaisesRegex(core.LauncherError, "already used"):
            self.profiles.create("nova")
        self.profiles.select(main)
        settings = self.profiles.settings
        self.assertEqual(settings["volume"], 25)
        game = core.installation(self.native)
        argv, env = core.launch_options(game, self.root, self.root / "cache", settings)
        self.assertIn("--fullscreen=1", argv)
        self.assertEqual(env["SWR_AUDIO_VOLUME"], "25")
        with self.assertRaises(core.LauncherError):
            core.launch_options(game, self.root, self.root / "cache", settings | {"size": "10x10"})

    def test_launch_layout_uses_active_profile_and_preserves_source_saves(self):
        first = self.profiles.active
        game = core.installation(self.native)
        original = game.saves / "user"
        original.mkdir(parents=True)
        (original / "AutoSave").write_bytes(b"GVAS original")
        first_run = core.session_directory(game, self.profiles)
        (first_run / core.DATA / "save/user").mkdir()
        (first_run / core.DATA / "save/user/AutoSave").write_bytes(b"GVAS active")
        self.profiles.create("Nova")
        second_run = core.session_directory(game, self.profiles)
        self.assertFalse((second_run / core.DATA / "save/user/AutoSave").exists())
        self.profiles.select(first)
        self.assertEqual((self.profiles.saves / "user/AutoSave").read_bytes(), b"GVAS active")
        core.prune_run_payload(first_run)
        core.prune_run_payload(second_run)
        self.assertEqual((original / "AutoSave").read_bytes(), b"GVAS original")

    def test_backup_export_restore_and_archive(self):
        active = self.profiles.active
        save = self.profiles.saves / "user/AutoSave"
        save.parent.mkdir()
        save.write_bytes(b"GVAS original")
        backup = self.profiles.backup()
        self.assertEqual((backup / "user/AutoSave").read_bytes(), b"GVAS original")
        out = self.profiles.export(self.root)
        save.write_bytes(b"GVAS changed")
        previous = self.profiles.restore(out)
        self.assertEqual((previous / "user/AutoSave").read_bytes(), b"GVAS changed")
        self.assertEqual(save.read_bytes(), b"GVAS original")
        with self.assertRaisesRegex(core.LauncherError, "at least one"):
            self.profiles.archive()
        self.profiles.create("Second")
        self.profiles.select(active)
        self.profiles.archive()
        archive = self.profiles.root / "archived-profiles" / active / "save/user/AutoSave"
        self.assertEqual(archive.read_bytes(), b"GVAS original")

    def test_interrupted_restore_recovers_previous_save(self):
        save = self.profiles.saves / "user/AutoSave"
        save.parent.mkdir()
        save.write_bytes(b"GVAS previous")
        backups = self.profiles.directory / "backups"
        backups.mkdir()
        self.profiles.saves.rename(backups / "BeforeRestore-test")
        reloaded = core.Profiles(self.profiles.root)
        self.assertEqual((reloaded.saves / "user/AutoSave").read_bytes(), b"GVAS previous")

    def test_launch_nonblocking_and_persists_normal_and_abnormal_exit(self):
        game = core.installation(self.native)
        popen = subprocess.Popen
        for expected in (0, 17):
            def stand_in(command, **kwargs):
                if isinstance(command, str):
                    return popen(command, **kwargs)
                self.assertEqual(command[0], str(kwargs["cwd"] / core.EXE))
                self.assertIn("--renderer=vulkan", command)
                return popen([sys.executable, "-c", f"import sys; print('fixture'); sys.exit({expected})"], **kwargs)
            with patch("launcher.core.subprocess.Popen", side_effect=stand_in):
                process, run = core.launch(game, self.profiles)
            code = process.wait(timeout=10)
            core.finish_session(run, code)
            self.assertEqual(code, expected)
            self.assertEqual(json.loads((run / "exit.json").read_bytes())["exit_code"], expected)
            self.assertIn("fixture", (run / "console.log").read_text())
            self.assertFalse((run / core.EXE).exists())

    def launched_ini(self, **settings):
        """The session's Orbitals.ini of a launch with these profile settings (the game is a stand-in process)."""
        self.profiles.update_active(settings=self.profiles.profile["settings"] | settings)
        game = core.installation(self.native)
        process, run = self.launch_stand_in(game)
        process.wait(timeout=10)
        return {line.partition("=")[0]: line.partition("=")[2] for line in (run / "Orbitals.ini").read_text().splitlines() if "=" in line}

    def launch_stand_in(self, game):
        popen = subprocess.Popen
        with patch("launcher.core.subprocess.Popen", side_effect=lambda c, **k: popen([sys.executable, "-c", "pass"], **k)):
            return core.launch(game, self.profiles)

    CONTROL_KEYS = {"button_layout", "gyro", "gyro_sensitivity_x", "gyro_sensitivity_y", "gyro_button", "gyro_button_mode", "gyro_calibrate"}

    def test_controls_defaults_select_playstation_buttons_and_icons(self):
        ini = self.launched_ini()
        self.assertEqual({key: ini[key] for key in self.CONTROL_KEYS & set(ini)}, {"button_layout": "playstation"})
        self.assertEqual(ini["button_accept_bottom"], "1")

    def test_button_layout_xbox_and_playstation(self):
        for layout, value in (("Xbox", "xbox"), ("PlayStation", "playstation")):
            ini = self.launched_ini(button_layout=layout)
            self.assertEqual(ini["button_layout"], value)
            self.assertEqual(ini["button_accept_bottom"], "1")
        ini = self.launched_ini(button_layout="Nintendo")
        self.assertNotIn("button_layout", ini)
        self.assertNotIn("button_accept_bottom", ini)

    def test_retired_checkbox_does_not_override_layout(self):
        for legacy in (True, False):
            for layout in ("Xbox", "PlayStation"):
                self.assertEqual(self.launched_ini(button_layout=layout, button_accept_bottom=legacy)["button_accept_bottom"], "1")
            self.assertNotIn("button_accept_bottom", self.launched_ini(button_layout="Nintendo", button_accept_bottom=legacy))

    def test_nintendo_clears_previous_game_ini_accept_override(self):
        (self.native / "Orbitals.ini").write_text("button_layout=playstation\nbutton_accept_bottom=1\n")
        ini = self.launched_ini(button_layout="Nintendo")
        self.assertNotIn("button_layout", ini)
        self.assertNotIn("button_accept_bottom", ini)

    def test_gyro_always_writes_the_five_keys(self):
        ini = self.launched_ini(gyro="Always", gyro_sensitivity_x=2.25, gyro_sensitivity_y=0.5, gyro_button="Right paddle",
                                gyro_button_mode="Press to toggle")
        self.assertEqual({key: ini[key] for key in self.CONTROL_KEYS - {"button_layout", "gyro_calibrate"}},
                         {"gyro": "always", "gyro_sensitivity_x": "2.25", "gyro_sensitivity_y": "0.50", "gyro_button": "paddle_right",
                          "gyro_button_mode": "toggle"})
        self.assertNotIn("gyro_calibrate", ini)
        self.assertEqual(self.launched_ini(gyro="While aiming (left trigger)")["gyro"], "aim")
        defaults = self.launched_ini(gyro="Always", gyro_sensitivity_x=1.5, gyro_button="None")
        self.assertEqual((defaults["gyro_sensitivity_x"], defaults["gyro_button"]), ("1.50", "none"))

    def test_gyro_off_writes_no_gyro_key_even_when_set_up(self):
        ini = self.launched_ini(gyro="Off", gyro_sensitivity_x=3.0, gyro_calibrate=True)
        self.assertFalse({key for key in ini if key.startswith("gyro")})
        self.assertTrue(self.profiles.settings["gyro_calibrate"])

    def test_gyro_calibrate_is_written_once(self):
        self.assertEqual(self.launched_ini(gyro="Always", gyro_calibrate=True)["gyro_calibrate"], "1")
        self.assertFalse(self.profiles.settings["gyro_calibrate"])
        self.assertEqual(core.Profiles(self.profiles.root).settings["gyro"], "Always")
        self.assertNotIn("gyro_calibrate", self.launched_ini())

    def test_controls_settings_are_validated(self):
        core.validate_settings(core.DEFAULT_SETTINGS | {"gyro_sensitivity_x": 4, "gyro_sensitivity_y": 0.5})
        for key, value in (("button_layout", "Sega"), ("gyro", "Sometimes"), ("gyro_button", "Select"), ("gyro_button_mode", "Hold"),
                           ("gyro_sensitivity_x", 0.25), ("gyro_sensitivity_y", 4.25), ("gyro_sensitivity_x", 1.6),
                           ("gyro_sensitivity_x", "1.5"), ("gyro_sensitivity_y", True), ("gyro_calibrate", 1), ("gyro_calibrate", None)):
            with self.subTest(key=key, value=value), self.assertRaises(core.LauncherError):
                core.validate_settings(core.DEFAULT_SETTINGS | {key: value})

    def test_old_control_keys_of_the_game_ini_are_replaced(self):
        old = "".join(f"{key}=old\n" for key in self.CONTROL_KEYS) + "gyro_extra=keep\n"
        (self.native / "Orbitals.ini").write_text(old)
        ini = self.launched_ini()
        self.assertEqual({key: ini[key] for key in self.CONTROL_KEYS & set(ini)}, {"button_layout": "playstation"})
        self.assertEqual(ini["gyro_extra"], "keep")
        game = core.installation(self.native)
        self.profiles.update_active(settings={"button_layout": "Xbox", "gyro": "Always"})
        text = (self.launch_stand_in(game)[1] / "Orbitals.ini").read_text()
        for key, value in (("button_layout", "xbox"), ("gyro", "always"), ("gyro_button", "touchpad")):
            self.assertEqual(text.count(f"\n{key}="), 1)
            self.assertIn(f"\n{key}={value}\n", text)
        self.assertNotIn("old", text)

    def test_os_process_error_keeps_active_saves(self):
        game = core.installation(self.native)
        original = subprocess.Popen

        def deny_game(command, **options):
            if isinstance(command, list) and command[0].endswith(core.EXE):
                raise PermissionError("blocked")
            return original(command, **options)

        with patch("launcher.core.subprocess.Popen", side_effect=deny_game):
            with self.assertRaises(PermissionError):
                core.launch(game, self.profiles)
        self.assertTrue(self.profiles.saves.is_dir())
        self.assertFalse((core.latest_run(self.profiles) / core.EXE).exists())


class ReportTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.profiles = core.Profiles(self.root / "launcher")
        self.game_path = self.root / "game"
        self.data = build_fixture(self.game_path)
        self.game = core.installation(self.game_path)
        self.logs = self.profiles.root / "logs"
        self.logs.mkdir()

    def test_manifests_log_redaction_integrity_and_no_network(self):
        (self.game_path / "Orbitals.log").write_text(
            "SwitchRecomp - Orbitals (runtime 0.4.1, built Nov 20 2026)\n"
            f"failure at {self.root} and {Path.home()}\nToken: sample-secret\n"
            "client 8.8.8.8; email player@example.org; ghp_fakestring000999\n",
            encoding="utf-8")
        (self.logs / "launcher.log").write_text("started", encoding="utf-8")
        with patch("socket.socket", side_effect=AssertionError("unexpected network access")):
            report = reports.create_report({"title": "Écran noir"}, {
                "game_logs": True, "launcher_logs": True, "version": True, "settings": True,
            }, self.profiles, self.game)
            path = report.write(self.root)
        with ZipFile(path) as bundle:
            self.assertIsNone(bundle.testzip())
            manifest = json.loads(bundle.read("manifest.json"))
            self.assertIn("logs/crash.log", manifest["missing"])
            self.assertIn("logs/launcher.log", bundle.namelist())
            self.assertFalse(any(name.startswith("saves/") for name in bundle.namelist()))
            for row in manifest["files"]:
                content = bundle.read(row["path"])
                self.assertEqual(len(content), row["bytes"])
                self.assertEqual(hashlib.sha256(content).hexdigest(), row["sha256"])
            log = bundle.read("logs/Orbitals.log").decode()
            for sensitive in (str(self.root), str(Path.home()), "sample-secret", "8.8.8.8",
                              "player@example.org", "ghp_fakestring000999"):
                self.assertNotIn(sensitive, log)
        self.assertIn("0.4.1", report.issue)
        self.assertEqual(path.with_name("issue.md").read_text(encoding="utf-8"), report.issue)

    def test_saves_require_selection_and_reject_game_data(self):
        user = self.profiles.saves / "user"
        user.mkdir()
        (user / "AutoSave").write_bytes(b"GVAS valid fixture")
        (user / "game.dll").write_bytes(b"MZ\x00\x00 proprietary fixture")
        (user / "prod.keys").write_bytes(b"key")
        name = Path("user/AutoSave")
        self.assertEqual(reports.report_saves(self.profiles.saves), {name: 18})
        bypass = reports.create_report({}, {}, self.profiles, self.game, selected_saves=(name,))
        self.assertNotIn("saves/user/AutoSave", bypass.files)
        report = reports.create_report({}, {"saves": True}, self.profiles, self.game, selected_saves=(name,))
        self.assertEqual(report.files["saves/user/AutoSave"], (user / "AutoSave").read_bytes())
        with self.assertRaises(core.LauncherError):
            reports.create_report({}, {"saves": True}, self.profiles, self.game,
                                  selected_saves=(Path("user/game.dll"),))

    def test_missing_logs_empty_form_and_hardware(self):
        with patch("launcher.reports.gpu_info", return_value={}), patch(
                "launcher.reports.system_info", return_value={"os": "Fixture", "os_release": "0", "architecture": "x64"}):
            report = reports.create_report({}, {"hardware": True, "game_logs": True, "version": True},
                                           self.profiles)
        self.assertIn("To be filled in", report.issue)
        self.assertIn("Fixture", report.issue)
        self.assertIn("system/gpu_info.json", report.missing)
        self.assertIn("logs/Orbitals.log", report.missing)
        self.assertNotIn("logs/Orbitals.log", report.files)

    def test_log_size_and_total_size(self):
        (self.logs / "launcher.log").write_bytes(b"head\n" + b"x" * reports.LOG_LIMIT + b"\ntail")
        report = reports.create_report({}, {"launcher_logs": True}, self.profiles)
        data = report.files["logs/launcher.log"]
        self.assertTrue(data.startswith(b"head"))
        self.assertTrue(data.endswith(b"tail"))
        self.assertIn(b"middle omitted", data)
        self.assertLess(len(data), reports.LOG_LIMIT + 100)
        with patch("launcher.reports.REPORT_LIMIT", 500):
            with self.assertRaisesRegex(core.LauncherError, "24 MB"):
                reports.create_report({}, {"launcher_logs": True}, self.profiles)


if __name__ == "__main__":
    unittest.main()
