# SwitchRecomp - Copyright 2026 MZ Labs
"""The settings pages: graphics, controls and general. A change is saved as it is made (Launcher.keep) and applies at the next launch."""

import re

from . import dialogs, graphics, navigation
from .core import (BUTTON_LAYOUTS, DEFAULT_SETTINGS, GYRO_BUTTON_MODES, GYRO_BUTTONS, GYRO_MODES, GYRO_SENSITIVITIES, LOG, WINDOWS,
                   LauncherError, gamepad_inventory, graphics_settings)
from .look import native_size
from .neo import motion_allowed
from .rows import Rows

PRESET_OF_MODE_LABELS = {"TV": "1920×1080 (TV)", "Portable": "1600×900 (handheld)"}
REMOVED = object()   # a setting taken out of the profile: its default again
DISPLAY_KEYS = ("fps", "mode", "display", "size", "upscale", "vsync", "tv_resolution")


def mode_label(mode):
    return PRESET_OF_MODE_LABELS.get(mode, mode.replace("x", "×"))


def valid_size(text):
    match = re.fullmatch(r"\s*(\d{3,4})\s*[x×]\s*(\d{3,4})\s*", text)
    if not match or not (320 <= int(match[1]) <= 8192 and 240 <= int(match[2]) <= 8192):
        raise LauncherError("Enter a size such as 1280x720 (320x240 to 8192x8192).")
    return f"{int(match[1])}x{int(match[2])}"


class SettingsPage:
    """What the three pages share: the rows, and reading and keeping one setting."""

    def __init__(self, app):
        self.app = app
        app.settings_note()
        self.rows = Rows(app, app.side)
        self.build()
        self.rows.ready()

    def build(self):
        raise NotImplementedError

    def get(self, name):
        return lambda: self.app.settings()[name]

    def keep(self, name):
        return lambda value: self.app.keep(**{name: value})

    def free(self):
        """Not while a game runs: its own PC settings screen is in charge, and is read when it ends."""
        return self.app.process is None

    def default(self, name):
        return (DEFAULT_SETTINGS[name],)

    def setting(self, name):
        return {"get": self.get(name), "set_": self.keep(name), "default": self.default(name)}


class GraphicsPage(SettingsPage):
    """Presets and options of launcher/graphics.py (only measured ones), frame pacing, display."""

    def build(self):
        rows, setting, free = self.rows, self.setting, self.free
        self.undo = None
        rows.group("Quality")
        rows.choice("Preset", graphics.PRESET_ORDER, self.get("gfx_preset"), self.set_preset,
                    lambda: graphics.PRESET_NOTES[self.app.settings()["gfx_preset"]] + "\n\nChanging a row under it makes it Custom.",
                    default=self.default("gfx_preset"), enabled=free)
        rows.choice("Internal 3D resolution", graphics.RENDER_MODES, self.mode, self.set_mode,
                    lambda: graphics.MODE_NOTES[self.mode()] + "\n\nThis is what changes the frame rate most.",
                    show=mode_label, default=("TV",), indent=True, enabled=free)
        for name, (caption, values) in graphics.OPTIONS.items():
            rows.choice(caption, tuple(values), lambda n=name: graphics.resolve(self.options())[n], lambda v, n=name: self.set_option(n, v),
                        f"{caption}. Original is the game's own.", default=(graphics.ORIGINAL,), indent=True, enabled=free)
        rows.group("Frame rate")
        rows.choice("Frame rate", graphics.FPS_CHOICES, describe=self.fps_text, enabled=free, **setting("fps"))
        rows.switch("VSync", describe="The picture waits for the monitor, so it does not tear. Off: the lowest delay.",
                    enabled=free, **setting("vsync"))
        rows.group("Display")
        rows.choice("TV resolution", graphics.TV_CHOICES, describe=graphics.TV_NOTE, enabled=free, **setting("tv_resolution"))
        rows.choice("Display", ("Windowed", "Borderless"), describe="Borderless fills the monitor at the desktop's resolution.",
                    enabled=free, **setting("display"))
        rows.choice("Window size", lambda: graphics.output_sizes(native_size(self.app.root)), describe=self.size_text,
                    show=lambda size: size.replace("x", "×"), other=("Other size…", self.other_size),
                    enabled=lambda: free() and self.app.settings()["display"] == "Windowed", **setting("size"))
        rows.choice("Upscaling filter", ("Standard", "Sharp", "FSR1"), describe="How the picture is scaled to the window or the monitor. "
                    "FSR1 is AMD's edge-aware filter, used when the window is larger than the picture.", enabled=free, **setting("upscale"))
        rows.group("Defaults")
        rows.action("All of this page", lambda: "Undo reset" if self.undo is not None else "Reset", self.reset,
                    "Reset puts every row of this page back to its default: High, 30 FPS, a 1280×720 window. It can be undone until you "
                    "leave the page.", enabled=free)

    def options(self):
        return graphics_settings(self.app.settings())

    def mode(self):
        settings = self.app.settings()
        return graphics.effective_mode(settings["gfx_preset"], settings["mode"])

    def set_preset(self, preset):
        values = {"gfx_preset": preset, "mode": graphics.effective_mode(preset, self.mode())}
        if preset != "Custom":
            values |= {f"gfx_{name}": REMOVED for name in graphics.OPTIONS}
        self.app.keep(**values)

    def set_mode(self, mode):
        self.app.keep(gfx_preset="Custom", mode=mode, **self.resolved())

    def set_option(self, name, value):
        self.app.keep(gfx_preset="Custom", mode=self.mode(), **self.resolved() | {f"gfx_{name}": value})

    def resolved(self):
        return {f"gfx_{name}": value for name, value in graphics.resolve(self.options()).items()}

    def fps_text(self):
        fps = self.app.settings()["fps"]
        if fps == graphics.FPS_CHOICES[0]:
            return graphics.FPS_WARNINGS[fps]
        return graphics.FPS_WARNINGS[graphics.FPS_CHOICES[0]], "Experimental. " + graphics.FPS_WARNINGS[fps]

    def size_text(self):
        text = "The size of the game's window. The 3D is drawn at the internal resolution, then scaled to it."
        return (text, "Borderless uses the monitor's desktop resolution.") if self.app.settings()["display"] == "Borderless" else text

    def other_size(self):
        text = dialogs.ask_text(self.app.root, "Window size", "Width x height, for example 1440x900", self.app.settings()["size"], valid_size)
        if text is not None:
            self.app.action(lambda: self.app.keep(size=valid_size(text)))

    def reset(self):
        self.app.flush()
        profiles = self.app.profiles
        current = dict(profiles.profile["settings"])
        if self.undo is not None:
            profiles.update_active(settings=self.undo)
            self.undo = None
            self.app.page_state.set("Graphics: the earlier settings are back.")
            return
        self.undo = current
        profiles.update_active(settings={key: value for key, value in current.items()
                                         if not key.startswith("gfx_") and key not in DISPLAY_KEYS})
        self.app.page_state.set("Graphics reset: High, 30 FPS.")


class ControlsPage(SettingsPage):
    CAMERAS = {None: "Game default", True: "Mouse + IJKL", False: "IJKL only"}

    def build(self):
        rows, setting, free = self.rows, self.setting, self.free
        self.found = "Close the game to scan for controllers." if self.app.process else "Scanning…"
        rows.group("Controllers")
        self.scan_row = rows.action("Connected controllers", "Scan again", self.scan, lambda: self.found, enabled=free)
        if WINDOWS:
            rows.switch("Controller vibration", describe="Rumble, on controllers that have it.", enabled=free, **setting("vibration"))
        rows.choice("Button icons", tuple(BUTTON_LAYOUTS), enabled=free, **setting("button_layout"),
                    describe="The controller drawn in the game's button prompts. The buttons keep their places: accept stays on the "
                             "right button (Circle / B), and the icons show it.")
        rows.switch("Accept with the bottom button", enabled=lambda: free() and self.app.settings()["button_layout"] != "Nintendo",
                    **setting("button_accept_bottom"),
                    describe="Experimental: Cross / A accepts and Circle / B goes back in the lobby, the pause menu and in game, as in the "
                             "PC version. The title screen and the main menu still accept with the right button.")
        rows.group("Gyro aiming")
        gyro_on = lambda: free() and self.app.settings()["gyro"] != "Off"
        rows.choice("Gyro aiming", tuple(GYRO_MODES), enabled=free, **setting("gyro"),
                    describe="Aim with the controller's motion sensor, on controllers that have one.")
        for name, key in (("Horizontal sensitivity", "gyro_sensitivity_x"), ("Vertical sensitivity", "gyro_sensitivity_y")):
            rows.choice(name, GYRO_SENSITIVITIES, show=lambda v: f"{v:.2f}", enabled=gyro_on, **setting(key),
                        describe="Camera degrees per degree the controller turns.")
        rows.choice("Gyro button", tuple(GYRO_BUTTONS), enabled=gyro_on, **setting("gyro_button"),
                    describe="The button that pauses the gyro or toggles it, as set in Button action.")
        rows.choice("Button action", tuple(GYRO_BUTTON_MODES), enabled=gyro_on, **setting("gyro_button_mode"),
                    describe="Hold to pause: the gyro stops while the button is held. Press to toggle: each press turns it off or on.")
        rows.switch("Calibrate at next launch", enabled=gyro_on, **setting("gyro_calibrate"),
                    describe="Put the controller down on a table for 2 seconds after the game starts; the calibration is stored per "
                             "controller model. Holding the gyro button 3 s with the controller lying still also calibrates.")
        rows.group("Keyboard and mouse")
        rows.choice("Assigned player", (1, 2), show=lambda n: f"Player {n}", enabled=free, **setting("keyboard"),
                    describe="Choose Player 2 for co-op with one controller. Keyboard and mouse take over Player 1 when its controller "
                             "is absent.")
        rows.choice("Right stick", tuple(self.CAMERAS), show=self.CAMERAS.get, enabled=free, **setting("mouse_camera"),
                    describe="What moves the right stick (the camera): the mouse and the IJKL keys, or the keys alone.\n\nF2 toggles the "
                             "mouse capture in game. Left click is ZR and right click is ZL.")
        rows.choice("Mouse speed", (None, *range(10, 501, 5)), show=lambda v: "Game default" if v is None else f"{v} %", enabled=free,
                    describe="How far the mouse moves the right stick. Game default uses the game's own setting.",
                    **setting("mouse_sensitivity"))
        if self.app.process is None:
            self.scan()

    def scan(self):
        game, root = self.app.game, self.scan_row
        self.found = "Scanning…"
        self.rows.show()

        def ask():
            try:
                return gamepad_inventory(game)
            except LauncherError as error:
                return None, str(error)

        def answer(result):
            if not root.winfo_exists():   # another page is shown
                return
            backend, devices = (None, "Could not check the connected controllers.") if isinstance(result, Exception) else result
            if backend is None:
                self.found = devices
            elif backend == "unavailable":
                self.found = "Controller support is unavailable on this system."
            elif devices:
                entries = [name if mapped else f"{name} · no gamepad mapping" for name, mapped in devices[:6]]
                if len(devices) > 6:
                    entries.append(f"and {len(devices) - 6} more")
                self.found = f"Found with {backend}:\n" + "\n".join("• " + entry for entry in entries)
            else:
                self.found = f"No controllers detected ({backend}). Connect one, then scan again."
            self.rows.show()

        self.app.probe(ask, answer)


class GeneralPage(SettingsPage):
    def build(self):
        rows, app = self.rows, self.app
        rows.group("Sound")
        rows.choice("Volume", tuple(range(0, 101, 5)), show=lambda v: f"{v} %", describe="The game's volume.", enabled=self.free,
                    **self.setting("volume"))
        rows.group("Launcher")
        rows.switch("Title screen animation", lambda: bool(app.profiles.data.get("animate", True)), lambda v: app.profiles.update(animate=v),
                    lambda: "The stars of the title screen. They never move while the game runs."
                    + ("" if motion_allowed() else "\n\nYour system's animations are off, so they stay still."), default=(True,))
        rows.choice("Controller in the launcher", tuple(navigation.MODES), lambda: app.profiles.data.get("pad_navigation", "auto"),
                    lambda mode: app.profiles.update(pad_navigation=mode),
                    "A controller drives the launcher: the directions choose, A takes, B goes back, the shoulders change the tab, Start plays.\n\n"
                    "Automatic is on, but not on the Steam Deck's desktop, where Steam already turns its controls into keys and a mouse.",
                    show=navigation.MODES.get, default=("auto",))
        rows.group("Updates")
        rows.switch("Check for updates at start", lambda: bool(app.profiles.data.get("update_check", True)),
                    lambda v: app.profiles.update(update_check=v),
                    "One request to GitHub when the launcher starts. Nothing is downloaded until you accept it.", default=(True,))
        rows.group("Problem reports")
        rows.entry("GitHub issues page", lambda: app.profiles.data.get("issues", ""), self.set_issues,
                   "Where TOOLS' GitHub issues goes. Reports are never sent by the launcher.")

    def set_issues(self, text):
        url = text.rstrip("/")
        if url and not re.fullmatch(r"https://github\.com/[\w.-]+/[\w.-]+/issues", url):
            raise LauncherError("Enter a GitHub address ending in /issues.")
        self.app.profiles.update(issues=url)
        LOG.info("event=issues_url_set")
