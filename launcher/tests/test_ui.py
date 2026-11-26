# Orbitals PC Recomp, a work based on SwitchRecomp by MZ Labs. GNU AGPL v3.0 with the additional terms of NOTICE.txt (LICENSE.txt).
"""The launcher window itself: every page is built once, with no game installed and with a fixture installation.

A page that raises while it is built (v0.1.5-preview.1: the home page's shader line passed justify twice) leaves the launcher closed
for the player; the core tests do not build any widget. Skipped where Tk has no display."""
import ctypes
import shutil
import tempfile
import threading
import time
import tkinter as tk
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from launcher import core, desktop, dialogs, graphics, look, page_settings, page_tools, report_form, rows, ui
from launcher.neo import TitleScreen
from launcher.tasks import Stopped
from launcher.ui import Launcher
from launcher.tests.test_launcher import build_fixture

PAGES = ("Home", "Saves", "Graphics", "Controls", "Settings", "Files", "Tools")


HOVERED = rows.Row.hovered


class LauncherWindow(unittest.TestCase):
    def setUp(self):
        try:
            self.tk = tk.Tk()
        except tk.TclError as error:
            self.skipTest(f"no display for Tk: {error}")
        self.tk.withdraw()
        p = patch.object(rows.Row, "hovered", lambda *_: None); p.start(); self.addCleanup(p.stop)   # (the mouse of whoever runs the tests)
        self.root = Path(tempfile.mkdtemp(prefix="orbitals-ui-"))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.addCleanup(self.tk.destroy)
        # no network while the pages are built
        for name in ("check_update",):
            p = patch.object(ui, name, return_value=None); p.start(); self.addCleanup(p.stop)

    def window(self, size="1160x755", game=True):
        """A launcher with a fixture installation, shown, with no update check."""
        if game:
            game = self.root / "game"
            if not game.exists():
                build_fixture(game)
        local = self.root / "local"
        local.mkdir(exist_ok=True)
        for p in (patch.object(Launcher, "update_result", lambda *a, **k: None), patch.object(ui, "detect_installation", return_value=None)):
            p.start(); self.addCleanup(p.stop)
        logs = core.setup_logging(local)
        logs.__enter__(); self.addCleanup(logs.__exit__, None, None, None)
        app = Launcher(self.tk, local, game or None)
        self.tk.geometry(size)
        self.tk.deiconify()
        self.spin(0.1)
        return app

    def spin(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.tk.update()
            time.sleep(0.01)

    def until(self, condition, seconds=20):
        """Wait for something, however busy the computer is."""
        end = time.monotonic() + seconds
        while not condition():
            self.assertLess(time.monotonic(), end, "it did not come")
            self.tk.update()
            time.sleep(0.01)
        self.spin(0.05)

    def title(self, app):
        return next(w for w in app.page.winfo_children() if isinstance(w, TitleScreen))

    def row(self, app, caption):
        return next(row for row in app.view.rows.items if row.caption == caption)


class LauncherWindowTest(LauncherWindow):
    def build(self, game=None):
        local = self.root / "local"
        local.mkdir(exist_ok=True)
        with core.setup_logging(local):
            app = Launcher(self.tk, local, game)
            for page in PAGES:
                with self.subTest(page=page, game=bool(game)):
                    app.show(page)
                    self.tk.update_idletasks()

    def test_every_page_builds_without_a_game(self):
        self.build()

    def test_every_page_builds_with_an_installation(self):
        native = self.root / "game"
        build_fixture(native)
        self.build(native)


    def test_title_entries_are_enabled_again_after_any_job(self):
        app = self.window()
        done = []
        go_on = threading.Event()
        app.background("Copying…", lambda: go_on.wait(20), done.append)
        self.spin(0.05)
        self.assertFalse(self.title(app).entries[0].enabled, "PLAY while a job runs")
        self.assertIsNone(self.title(app).card)
        go_on.set()
        self.until(lambda: done)
        self.assertTrue(self.title(app).entries[0].enabled, "PLAY after it")

    def controllers(self, app, *names):
        """As if these were connected, also for the watch that asks every second."""
        app.pad_names = list(names)
        p = patch.object(app.pads, "names", return_value=list(names)); p.start(); self.addCleanup(p.stop)

    def test_chips_and_controllers_lead_to_their_pages(self):
        app = self.window()
        self.controllers(app, "Pad")
        app.show("Home")
        for pick, page in ((lambda s: s.chips[0], "Files"), (lambda s: next(e for e in s.entries if e.label == "SAVES"), "Saves"), (lambda s: s.pads_spot, "Controls")):
            app.show("Home"); self.spin(0.1)
            screen = self.title(app)
            with patch.object(page_settings, "gamepad_inventory", return_value=("XInput", [])):
                screen.take(pick(screen))
            self.assertEqual(app.active_page, page)

    def test_the_controllers_line_comes_with_their_first_answer(self):
        app = self.window()
        app.pad_names = None
        app.show("Home")
        self.assertIsNone(self.title(app).pads_spot)
        self.tk.after_cancel(app.pads_timer)
        with patch.object(app.pads, "names", return_value=["Pad"]):
            app.watch_pads()
            self.assertEqual(self.title(app).pads[0], "Pad")

    def test_the_arrows_reach_everything_on_the_title_screen(self):
        app = self.window()
        self.controllers(app, "Pad", "Other pad")
        app.show("Home"); self.spin(0.2)
        screen = self.title(app)
        self.assertEqual(screen.pads[0], "2 controllers: Pad, Other pad")
        reached, todo = {screen.selected}, [screen.selected]
        while todo:
            start = todo.pop()
            for x, y in ((0, -1), (0, 1), (-1, 0), (1, 0)):
                screen.selected = start
                screen.go(x, y)
                if screen.selected not in reached:
                    reached.add(screen.selected); todo.append(screen.selected)
        self.assertEqual(reached, {spot for spot in screen.spots if spot.enabled})
        screen.selected = screen.buttons[0]
        screen.go(1, 0); self.assertIs(screen.selected, screen.buttons[1])

    def test_cards_say_the_most_important_thing_once(self):
        app = self.window()
        self.assertEqual(app.card().title, "Shaders are not prepared")
        with patch.object(ui, "outdated_release", return_value="0.1.0"):
            self.assertEqual(app.card().title, "Your game can be updated")
            app.keep_old_game()
            self.assertEqual(app.card().title, "Shaders are not prepared")
        app.show_update({"latest": "v9.0.0", "newer": True, "url": "https://github.com/x", "notes": "", "assets": {}}, None, True)
        self.assertEqual(self.title(app).card.title, "Version v9.0.0 is available")
        app.not_now()
        self.assertEqual(app.card().title, "Shaders are not prepared")
        app.skip_shaders()
        self.assertIsNone(app.card())

    def test_a_failed_long_job_is_a_card_with_its_retry(self):
        app = self.window()
        tried = []

        def fails():
            raise core.LauncherError("The disk is full.")

        with patch.object(ui.dialogs, "error") as dialog:
            app.background("Installing…", fails, None, demo="RECOMPILING ORBITALS", retry=lambda: tried.append(1))
            self.until(lambda: app.notice)
        dialog.assert_not_called()
        card = self.title(app).card
        self.assertTrue(card.alarm)
        self.assertEqual((card.title, card.text), ("Recompiling Orbitals failed", "The disk is full."))
        self.assertEqual([label for label, _ in card.buttons], ["Try again", "Open logs", "Dismiss"])
        card.buttons[0][1]()
        self.assertEqual(tried, [1])
        self.assertIsNone(app.notice)

    def buttons(self, widget):
        found = {}
        for child in widget.winfo_children():
            if child.winfo_class() == "TButton":
                found[child.cget("text").capitalize()] = child
            found |= self.buttons(child)
        return found

    def test_files_page_takes_a_copy_and_installs_it(self):
        app = self.window(game=False)
        app.show("Files"); self.spin(0.1)
        self.assertTrue(self.buttons(app.page)["Install"].instate(["disabled"]))
        holder = self.root / "extracted"   # holds the copy, as some tools make it
        (holder / "game" / "exefs").mkdir(parents=True)
        app.set_source(holder)
        self.assertEqual(app.source(), holder, "not a copy yet")
        (holder / "game" / "romfs").mkdir()
        for name in ("main", "main.npdm"):
            (holder / "game" / "exefs" / name).write_bytes(b"x")
        app.set_source(holder)
        self.assertEqual(app.source(), holder / "game")
        app.refresh(); self.spin(0.1)
        install = self.buttons(app.page)["Install"]
        self.assertTrue(install.instate(["!disabled"]))
        native = self.root / "game"
        build_fixture(native)

        go_on = threading.Event()

        def setup(source, on_progress, task, rebuild=False):
            on_progress(4, 5, "compiled 100/400 (1s)")
            go_on.wait(20)
            return core.installation(native)

        with patch.object(ui, "install_from_files", side_effect=setup):
            install.invoke()
            self.spin(0.1)
            self.assertEqual(app.active_page, "Home")   # its demo
            self.assertIn("Stop", self.buttons(app.page))
            app.show("Files")
            self.until(lambda: app.install_step == 4)
            self.until(lambda: [line.cget("fg") for _, line in app.view.steps] == [look.CYAN] * 3 + [look.INK, look.MUTED])
            go_on.set()
            self.until(lambda: app.job is None)
        self.assertEqual(app.game.root, native.resolve())
        self.assertEqual((app.active_page, app.card().title), ("Home", "Shaders are not prepared"))

    def test_files_page_shows_a_failure_with_its_retry(self):
        app = self.window()
        app.show("Files")
        with patch.object(ui, "install_from_files", side_effect=core.LauncherError("The installation failed (code 1).")):
            app.set_source(self.root / "game")
            app.start_install()
            app.show("Files")
            self.until(lambda: app.job is None)
        self.assertEqual(app.active_page, "Files")
        self.assertIn("Try again", self.buttons(app.page))

    def test_a_long_job_can_be_stopped_and_closing_asks(self):
        app = self.window()
        app.set_source(self.root / "game")

        def setup(source, on_progress, task, rebuild=False):
            while not task.stopped:
                time.sleep(0.02)
            raise Stopped("The installation was stopped. What is compiled is kept.")

        with patch.object(ui, "install_from_files", side_effect=setup), patch.object(ui.dialogs, "confirm", return_value=False) as ask:
            app.start_install(); self.spin(0.1)
            app.stop_job()
            self.assertFalse(app.task.stopped, "not confirmed")
            app.close()
            self.assertTrue(self.tk.winfo_exists())
            ask.return_value = True
            app.stop_job()
            self.until(lambda: app.job is None)
        self.assertIsNone(app.notice, "a stop is not a failure")
        self.assertIn("kept", app.page_state.get())
        self.assertEqual(app.card().title, "Shaders are not prepared")

    def test_a_stop_that_leaves_the_game_incomplete_says_so(self):
        app = self.window()
        (self.root / "game" / core.MODULE).unlink()
        app.check_game()
        self.assertIsNone(app.game)
        self.assertEqual(app.card().title, "The game is not complete")

    def test_saves_without_profiles_and_those_of_an_earlier_version(self):
        app = self.window()
        app.show("Saves")
        captions = [row.caption for row in app.view.rows.items]
        self.assertNotIn("Saves in use", captions)
        first = app.profiles.active
        app.profiles.create("Second")   # as an earlier launcher left it
        app.show("Saves")
        saves = self.row(app, "Saves in use")
        self.assertEqual(saves.value.cget("text"), "Second")
        saves.move(-1)
        self.assertEqual(app.profiles.active, first)

    def test_the_selected_row_is_the_lit_one_while_the_keys_are_on_it(self):
        app = self.window()
        app.show("Tools"); self.spin(0.2)
        self.tk.focus_force(); self.spin(0.1)
        rows = app.view.rows
        rows.selected.focus_set(); self.spin(0.1)
        self.assertEqual([row.caption for row in rows.items if row.chosen], [rows.selected.caption])
        app.focus_header(); self.spin(0.1)
        self.assertFalse(rows.selected.keyed)

    def test_a_job_that_ends_leaves_the_player_where_they_were(self):
        app = self.window()
        app.show("Saves")
        app.view.rows.choose("Back up")
        app.background("Copying…", lambda: time.sleep(0.1), lambda _: None)
        self.until(lambda: app.job is None)
        self.assertEqual(app.view.rows.selected.caption, "Back up")
        app.show("Settings")
        row = self.row(app, "GitHub issues page")
        row.entry.delete(0, "end"); row.entry.insert(0, "https://github.com/someone/project/issues")
        app.refresh()
        self.assertEqual(app.profiles.data["issues"], "https://github.com/someone/project/issues")

    def test_add_to_steam_says_what_it_did(self):
        app = self.window()
        app.show("Tools")
        with patch.object(page_tools.steam, "add", side_effect=core.LauncherError("Steam was not found on this PC.")) as add:
            app.view.rows.choose("Add to Steam")
            self.row(app, "Add to Steam").press()
            self.until(lambda: "Adding…" not in self.row(app, "Add to Steam").describe())
        self.assertEqual(set(add.call_args.args[0]), {"p.png", ".png", "_hero.png", "_logo.png"})
        self.assertIn("not found", self.row(app, "Add to Steam").describe())

    def test_a_long_card_stays_above_the_chips(self):
        app = self.window()
        for size in ("880x650", "1160x755"):
            self.tk.geometry(size)
            app.fail("A failure with quite a long title that does not fit", "One long explanation of it. " * 30, ("Try again", lambda: None),
                     ("Open logs", lambda: None))
            app.show("Home"); self.spin(0.2)
            screen = self.title(app)
            self.assertLess(screen.card_box[3], screen.chips[0].box[1])
            for spot in screen.buttons:
                self.assertLess(spot.box[2], screen._split(spot.box[3]))

    def test_controller_scan_neither_blocks_saving_nor_loops_on_errors(self):
        app = self.window()
        with patch.object(page_settings, "gamepad_inventory", side_effect=RuntimeError("unexpected")) as scan, \
                patch.object(ui.dialogs, "error") as dialog:
            app.show("Controls")
            self.assertIsNone(app.job)
            app.keep(keyboard=2)
            self.until(lambda: scan.call_count and not app.pending)
            self.spin(0.3)
        self.assertEqual(scan.call_count, 1)
        dialog.assert_not_called()
        self.assertEqual(app.profiles.settings["keyboard"], 2)

    def test_button_layout_choice_persists_and_locks_during_game(self):
        app = self.window()
        with patch.object(page_settings, "gamepad_inventory", return_value=("SDL3", [])):
            app.show("Controls")
            choice = self.row(app, "Button layout")
            self.assertNotIn("Accept with the bottom button", [row.caption for row in app.view.rows.items])
            for expected in ("Nintendo", "Xbox", "PlayStation"):
                choice.press()
                app.flush()
                self.assertEqual(core.Profiles(app.profiles.root).settings["button_layout"], expected)
            app.process = object()
            try:
                app.view.rows.sync()
                choice.press()
                self.assertEqual(app.settings()["button_layout"], "PlayStation")
            finally:
                app.process = None

    def test_settings_are_kept_as_they_change(self):
        app = self.window()
        app.show("Settings")
        self.row(app, "Volume").move(-1)
        self.row(app, "Volume").move(-1)
        self.assertEqual(self.row(app, "Volume").value.cget("text"), "90 %")
        app.show("Graphics")   # another tab, before they are written
        self.assertEqual(app.profiles.settings["volume"], 90)
        self.row(app, "VSync").press()
        self.until(lambda: not app.pending)
        self.assertFalse(app.profiles.settings["vsync"])
        self.assertEqual(core.Profiles(app.local).settings["volume"], 90)

    def test_a_preset_and_the_rows_under_it(self):
        app = self.window()
        app.show("Graphics")
        preset, resolution = self.row(app, "Preset"), self.row(app, "Internal 3D resolution")
        for name in graphics.PRESET_ORDER[:3]:
            preset.take(name)
            self.assertEqual(resolution.get(), graphics.PRESET_MODE[name])
        resolution.take("1600x900")
        self.assertEqual((preset.get(), resolution.get()), ("Custom", "1600x900"))
        app.flush()
        self.assertEqual(core.graphics_settings(app.profiles.settings)["preset"], "Custom")
        self.row(app, "All of this page").press()   # reset, then its undo
        self.assertEqual((preset.get(), app.profiles.profile["settings"]), ("High", {}))
        self.row(app, "All of this page").press()
        self.assertEqual((preset.get(), resolution.get()), ("Custom", "1600x900"))

    def test_a_value_of_its_own_is_shown_and_left(self):
        app = self.window()
        app.profiles.update_active(settings={"mouse_sensitivity": 147, "size": "1000x700"})   # from the game's own settings
        with patch.object(page_settings, "gamepad_inventory", return_value=("XInput", [])):
            app.show("Controls")
            self.assertEqual(self.row(app, "Mouse speed").value.cget("text"), "147 %")
            app.show("Graphics")
        self.assertEqual(self.row(app, "Window size").value.cget("text"), "1000×700")
        self.row(app, "Display").take("Borderless")
        self.assertFalse(self.row(app, "Window size").on)

    def test_rows_are_read_only_while_the_game_runs(self):
        app = self.window()
        app.process = object()
        app.show("Graphics")
        self.assertFalse(any(row.on for row in app.view.rows.items))
        self.row(app, "VSync").press()
        with self.assertRaises(core.LauncherError):
            app.keep(vsync=False)
        app.process = None
        self.assertTrue(app.profiles.settings["vsync"])

    def test_a_refused_text_is_said_under_its_field(self):
        app = self.window()
        app.show("Settings")
        row = self.row(app, "GitHub issues page")
        row.entry.delete(0, "end"); row.entry.insert(0, "https://example.com"); row.keep()
        self.assertIn("/issues", row.error.cget("text"))
        row.entry.delete(0, "end"); row.entry.insert(0, "https://github.com/someone/project/issues/"); row.keep()
        self.assertEqual((row.error.cget("text"), app.profiles.data["issues"]), ("", "https://github.com/someone/project/issues"))

    def test_the_keyboard_goes_through_rows_header_and_tabs(self):
        app = self.window()
        app.show("Graphics"); self.spin(0.1)
        rows = app.view.rows
        self.assertIs(rows.selected, rows.items[0])
        rows.step(rows.items[0], 1); self.spin(0.05)
        rows.step(rows.items[1], -1); self.spin(0.05)
        rows.step(rows.items[0], -1); self.spin(0.05)
        app.header._move(1)
        app.header.places[app.header.cursor]()
        self.assertEqual(app.active_page, "Controls")
        app.next_tab(1)
        self.assertEqual(app.active_page, "Settings")

    def test_every_page_can_be_seen_whole_at_the_minimum_size(self):
        app = self.window("880x650")
        with patch.object(page_settings, "gamepad_inventory", return_value=("XInput", [])):
            for page in PAGES[1:]:
                with self.subTest(page=page):
                    app.show(page); self.spin(0.15)
                    self.assertEqual(app.scroll.overflows(), bool(app.scroll.bar.winfo_ismapped()))
                    app.scroll.view.yview_moveto(1.0); self.tk.update()
                    self.assertGreaterEqual(app.scroll.view.canvasy(app.scroll.view.winfo_height()), app.page.winfo_reqheight() - 20)

    def test_title_texts_stay_in_their_panels(self):
        app = self.window()
        app.profiles.rename("A profile name of 32 characters")
        for size in ("880x650", "1160x755", "1600x900"):
            self.tk.geometry(size)
            app.show("Home"); self.spin(0.2)
            screen = self.title(app)
            with self.subTest(size=size):
                for item in screen.find_all():
                    if screen.type(item) != "text":
                        continue
                    left, top, right, bottom = screen.bbox(item)
                    self.assertLessEqual(right, screen.w, screen.itemcget(item, "text"))
                    self.assertLessEqual(bottom, screen.h, screen.itemcget(item, "text"))
                    if left < screen.w * 0.4 and "spots" not in screen.gettags(item):
                        self.assertLessEqual(right, screen._split((top + bottom) / 2) + 2, screen.itemcget(item, "text"))

    def test_chips_stay_left_of_the_diagonal(self):
        app = self.window("880x650")
        app.profiles.rename("A profile name of 32 characters")
        for game in (app.game, None):
            app.game = game
            app.show("Home"); self.spin(0.2)
            screen = self.title(app)
            for spot in screen.chips:
                self.assertLess(spot.box[2], screen._split(spot.box[3]))

    def test_pages_and_title_fit_at_150_percent(self):
        with patch.object(ui, "scale_to", lambda root: look.scale_to(root, dpi=144)):
            app = self.window("1320x975")
        self.addCleanup(setattr, look, "SCALE", 1.0)
        if look.SCALE < 1.5:
            self.skipTest("the screen is too small for 150 %")
        self.assertEqual(look.px(100), 150)
        self.spin(0.2)
        screen = self.title(app)
        for spot in screen.chips + screen.buttons:
            self.assertLess(spot.box[2], screen._split(spot.box[3]))
        self.assertLess(screen.entries[-1].box[3], screen.h)
        with patch.object(page_settings, "gamepad_inventory", return_value=("XInput", [])):
            for page in PAGES[1:]:
                app.show(page); self.spin(0.1)
                self.assertGreaterEqual(app.page.winfo_width(), app.page.winfo_reqwidth(), page)

    def test_the_scale_never_outgrows_the_screen(self):
        with patch.object(self.tk, "winfo_screenwidth", return_value=1280), patch.object(self.tk, "winfo_screenheight", return_value=800):
            look.scale_to(self.tk, dpi=215)   # a small dense screen
        self.addCleanup(setattr, look, "SCALE", 1.0)
        self.assertLessEqual(look.px(650), 800 - 72 + 1)
        self.assertLessEqual(look.px(880), 1280)
        look.scale_to(self.tk, dpi=72)
        self.assertEqual(look.SCALE, 1.0)

    def test_moving_the_mouse_repaints_nothing(self):
        app = self.window()
        screen = self.title(app)
        with patch.object(screen, "configure") as configure:   # any configure repaints a whole canvas
            for x in range(5, 60):
                screen._hover(SimpleNamespace(x=x, y=5))
        configure.assert_not_called()

    @unittest.skipUnless(core.WINDOWS, "the Windows taskbar")
    def test_the_window_windows_shows_has_the_icon_and_the_identity(self):
        self.window()   # (Tk makes the real window when it is shown: what was set before is not on it)
        self.spin(0.4)
        user = ctypes.windll.user32
        user.SendMessageW.restype = ctypes.c_void_p
        user.SendMessageW.argtypes = (ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p)
        for full in (False, True, False):
            self.tk.attributes("-fullscreen", full); self.spin(0.5)
            frame = int(self.tk.wm_frame(), 16)
            for which in (0, 1):
                self.assertTrue(user.SendMessageW(frame, 0x7f, which, None), f"icon {which}, full screen {full}")   # WM_GETICON
            self.assertEqual(desktop.window_identity(frame), desktop.APP_ID)

    def test_the_shaders_text_keeps_its_commas(self):
        app = self.window()
        with patch.object(ui, "shader_preparation", return_value={"discovered": 23134, "validated": 23130, "failed": 4, "seconds": 900,
                                                                  "at": "2026-11-25T18:00:00"}):
            text = app.shader_state_text()
        self.assertIn("23 130 prepared and validated of 23 134, 4 not supported by the translator (2026-11-25, 15 min).", text)

    def test_a_failure_is_dismissed_on_game_files_too(self):
        app = self.window()
        app.fail("It failed", "Why.")
        app.show("Files")
        self.buttons(app.page)["Dismiss"].invoke()
        self.assertNotIn("Dismiss", self.buttons(app.page))

    def test_settings_that_cannot_be_written_do_not_hold_the_player(self):
        app = self.window()
        app.show("Graphics")
        app.keep(vsync=False)
        with patch.object(app.profiles, "update_active", side_effect=PermissionError("in use")), patch.object(ui.dialogs, "error") as said:
            app.show("Home")
        said.assert_called_once()
        self.assertEqual(app.active_page, "Home")
        app.flush()   # and they are not lost
        self.assertFalse(app.profiles.settings["vsync"])

    def test_the_mouse_selects_a_row_when_it_moves_not_when_the_page_scrolls(self):
        app = self.window()
        app.show("Tools"); self.spin(0.2)
        self.tk.focus_force(); self.spin(0.1)
        first, second = app.view.rows.items[:2]
        first.focus_set(); self.spin(0.1)
        hovered = HOVERED   # (as the tests' own window has it turned off)
        at = SimpleNamespace(x_root=app.view.rows.pointer[0], y_root=app.view.rows.pointer[1])
        hovered(second, at); self.spin(0.1)   # the page moved under a mouse that did not
        self.assertIs(app.view.rows.selected, first)
        hovered(second, SimpleNamespace(x_root=at.x_root + 3, y_root=at.y_root)); self.spin(0.1)
        if self.tk.focus_displayof() is not None:
            self.assertIs(app.view.rows.selected, second)

    def test_a_typed_report_is_not_closed_by_a_key(self):
        app = self.window()
        app.show("Tools"); self.spin(0.1)
        self.tk.focus_force(); self.spin(0.1)
        if self.tk.focus_displayof() is None:
            self.skipTest("the window cannot be brought to the front here")
        app.report(); self.spin(0.3)
        form = next(w for w in self.tk.winfo_children() if w.winfo_class() == "Toplevel")
        with patch.object(report_form.dialogs, "confirm", return_value=False) as ask:
            self.assertEqual(self.tk.focus_get().winfo_toplevel(), form, "the keys are the form's, not the page's under it")
            text = next(w for w in self.every(form) if w.winfo_class() == "Text")
            text.insert("1.0", "It stops.")
            self.tk.focus_get().event_generate("<Escape>"); self.spin(0.1)
            self.assertTrue(form.winfo_exists())
            ask.return_value = True
            self.tk.focus_get().event_generate("<Escape>"); self.spin(0.1)
        self.assertFalse(form.winfo_exists())

    def every(self, widget):
        for child in widget.winfo_children():
            yield child
            if child.winfo_class() != "TCombobox":
                yield from self.every(child)

    def test_the_arrows_pass_a_drop_down_list(self):
        app = self.window()
        app.report(); self.spin(0.2)
        form = next(w for w in self.tk.winfo_children() if w.winfo_class() == "Toplevel")
        box = next(w for w in self.every(form) if w.winfo_class() == "TCombobox")
        box.tk.call("ttk::combobox::PopdownWindow", box)   # opened once: it has a window Tkinter does not know
        button = next(w for w in self.every(form) if w.winfo_class() == "TButton")
        self.assertTrue(dialogs.toward(button, 1, 0, form))

    def test_title_screen_is_still_while_a_game_runs(self):
        app = self.window()
        screen = self.title(app)
        screen.moves = True
        with patch.object(screen, "focus_displayof", return_value=screen):
            self.assertTrue(screen._moving())
            app.process = object()
            self.assertFalse(screen._moving())
        app.process = None


class DemoScreenTest(unittest.TestCase):
    """Each long task's demo builds and draws frames (a Tk error in a frame would stop its animation)."""

    def test_every_demo_draws(self):
        try:
            root = tk.Tk()
        except tk.TclError as error:
            self.skipTest(f"no display for Tk: {error}")
        self.addCleanup(root.destroy)
        root.geometry("900x500")
        from launcher.demo import DemoScreen, style_for
        self.assertEqual([style_for(t) for t in ("RECOMPILING ORBITALS", "PREPARING SHADERS", "DOWNLOADING UPDATE")],
                         ["code", "shader", "stars"])
        for title in ("RECOMPILING ORBITALS", "PREPARING SHADERS", "DOWNLOADING UPDATE"):
            with self.subTest(title=title):
                demo = DemoScreen(root, title=title)
                demo.pack(fill="both", expand=True)
                demo.set_progress(0.5, "Step 2/5", "detail")
                root.update()
                demo._layout()
                for _ in range(3):
                    getattr(demo, "_frame_" + demo.style)()
                    demo._frame_common()
                    demo.t += 1
                demo.destroy()


if __name__ == "__main__":
    unittest.main()
