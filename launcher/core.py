# SwitchRecomp - Copyright 2026 MZ Labs

"""Local installations, save profiles, and native game processes."""

import hashlib
import json
import logging
import mmap
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from configparser import ConfigParser, Error as ConfigError
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime
from logging.handlers import RotatingFileHandler
from pathlib import Path
from uuid import uuid4

from . import VERSION, graphics
from .tasks import Stopped, Task

REPO = Path(__file__).resolve().parent.parent
WINDOWS = os.name == "nt"
EXE = "Orbitals.exe" if WINDOWS else "Orbitals"
MODULE = "Orbitals_code.dll" if WINDOWS else "Orbitals_code.so"
DATA = "Orbitals_data"
MODULE_NAMES = ("rtld", "main", "sdk", *(f"subsdk{i}" for i in range(10)))
PAKS = ("global.ucas", "global.utoc", "Orbitals-Switch2.pak",
        "Orbitals-Switch2.ucas", "Orbitals-Switch2.utoc")
PAK_DIR = Path("Orbitals/Content/Paks")
SAVE_KINDS = {"user", "system", "device", "bcat", "cache", "temp"}
SAVE_NAMES = {"AutoSave", "OrbInputUserSettings", "ProfileSave", "SharedGameSettings"}
ISSUES_URL = "https://github.com/Dimi-Sama/OrbitalsPCRecomp/issues"
DEFAULT_SETTINGS = {
    "mode": "Portable", "display": "Windowed", "size": "1280x720",
    "upscale": "Standard", "vsync": True, "volume": 100, "keyboard": 1,
    "mouse_camera": None, "mouse_sensitivity": None, "vibration": True,
    "fps": "30 (reference)", "gfx_preset": "High", "tv_resolution": "1080p (console)",
    "button_layout": "Nintendo", "gyro": "Off", "gyro_sensitivity_x": 1.5, "gyro_sensitivity_y": 1.5,
    "gyro_button": "Touchpad click", "gyro_button_mode": "Hold to pause", "gyro_calibrate": False,
}
# Orbitals.ini values of the controls settings; Nintendo is the game as it is.
BUTTON_LAYOUTS = {"Nintendo": None, "Xbox": "xbox", "PlayStation": "playstation"}
GYRO_MODES = {"Off": "off", "Always": "always", "While aiming (left trigger)": "aim"}
GYRO_BUTTONS = {"Touchpad click": "touchpad", "Mute button": "mute", "Left paddle": "paddle_left", "Right paddle": "paddle_right",
                "L3": "l3", "R3": "r3", "None": "none"}
GYRO_BUTTON_MODES = {"Hold to pause": "pause", "Press to toggle": "toggle"}
GYRO_SENSITIVITIES = tuple(step / 4 for step in range(2, 17))   # 0.5 to 4.0
# setting values saved by the launcher's earlier French interface, read as today's
LEGACY_VALUES = {"Fenêtré": "Windowed", "Sans bordure": "Borderless",
                 "30 (référence)": "30 (reference)", "60 (expérimental)": "60 (experimental)"}
LOG = logging.getLogger("orbitals")


class LauncherError(ValueError):
    """An error that can be shown to a player; `copy`: a command the launcher puts on the clipboard with it (a package install)."""

    def __init__(self, message, copy=None):
        super().__init__(message)
        self.copy = copy


def data_home():
    if WINDOWS:
        return Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData/Local") / "OrbitalsLauncher"
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local/share") / "orbitals-launcher"


@contextmanager
def instance_lock(root):
    root.mkdir(parents=True, exist_ok=True)
    with (root / ".lock").open("a+b") as lock:
        lock.seek(0)
        try:
            if WINDOWS:
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise LauncherError("The launcher is already open for this data folder.") from exc
        yield


@contextmanager
def setup_logging(root):
    directory = root / "logs"
    directory.mkdir(parents=True, exist_ok=True)
    handler = RotatingFileHandler(directory / "launcher.log", maxBytes=512_000,
                                  backupCount=3, encoding="utf-8")
    handler.setFormatter(logging.Formatter(
        "%(asctime)s %(levelname)s %(message)s", "%Y-%m-%dT%H:%M:%S%z"))
    LOG.setLevel(logging.INFO)
    LOG.addHandler(handler)
    LOG.info("event=start launcher=%s", VERSION)
    try:
        yield
    finally:
        LOG.removeHandler(handler)
        handler.close()


@contextmanager
def atomic_output(destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=destination.parent, prefix=".orbitals-")
    os.close(fd)
    temporary = Path(name)
    try:
        yield temporary
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def write_text(path, text):
    with atomic_output(path) as temporary:
        temporary.write_text(text, encoding="utf-8")


def timestamp():
    return datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid4().hex[:8]


def safe_entry(path):
    info = path.lstat()
    if (path.is_symlink() or getattr(info, "st_file_attributes", 0) &
            getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)):
        raise LauncherError(f"Link or junction refused ({path.name}).")
    if not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
        raise LauncherError(f"Unsupported file type ({path.name}).")
    return info


def tree_files(root, *, max_files=50_000):
    if not stat.S_ISDIR(safe_entry(root).st_mode):
        raise LauncherError(f"Select a folder ({root.name}).")
    files = {}
    stack = [root]
    seen = 0
    while stack:
        directory = stack.pop()
        for entry in sorted(directory.iterdir()):
            seen += 1
            if seen > max_files:
                raise LauncherError("This folder holds too many files.")
            info = safe_entry(entry)
            if stat.S_ISDIR(info.st_mode):
                stack.append(entry)
            else:
                files[entry.relative_to(root)] = info.st_size
    return files


@contextmanager
def read_local(root, relative):
    """Open a known relative path without traversing linked components below root."""
    if relative.anchor or not relative.parts or ".." in relative.parts:
        raise LauncherError("Invalid file path.")
    path = root
    safe_entry(root)
    for part in relative.parts:
        path /= part
        info = safe_entry(path)
    if not stat.S_ISREG(info.st_mode):
        raise LauncherError(f"A regular file was expected ({path.name}).")
    with path.open("rb") as stream:
        if not os.path.samestat(info, os.fstat(stream.fileno())):
            raise LauncherError(f"File changed while it was read ({path.name}).")
        yield stream


def copy_files(source, destination, files, progress=lambda _: None):
    written = 0
    for relative, size in files.items():
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with read_local(source, relative) as input_file, target.open("xb") as output_file:
            remaining = size
            while remaining:
                block = input_file.read(min(4 * 1024 * 1024, remaining))
                if not block:
                    raise LauncherError(f"The file changed ({relative.name}).")
                output_file.write(block)
                remaining -= len(block)
                written += len(block)
                progress(written)
            if input_file.read(1):
                raise LauncherError(f"The file changed ({relative.name}).")


def build_ids(exefs):
    ids = {}
    for name in MODULE_NAMES:
        path = exefs / name
        if path.is_file():
            with path.open("rb") as file:
                header = file.read(0x60)
            if len(header) < 0x60 or header[:4] != b"NSO0" or not any(header[0x40:0x60]):
                raise LauncherError(f"Unreadable ExeFS module ({name}).")
            ids[name] = header[0x40:0x60].hex()
    if not {"main", "rtld", "sdk"}.issubset(ids) or not (exefs / "main.npdm").is_file():
        raise LauncherError("Incomplete ExeFS: Orbitals needs main, rtld, sdk and main.npdm.")
    return ids


def validate_romfs(romfs):
    paks = romfs / PAK_DIR
    if not all((paks / name).is_file() and (paks / name).stat().st_size for name in PAKS):
        raise LauncherError("Incomplete RomFS: the Orbitals-Switch2 and global containers are needed.")


def display_version(root):
    for relative in ("control.nacp", "control/control.nacp"):
        try:
            with read_local(root, Path(relative)) as file:
                file.seek(0x3060)
                value = file.read(16).split(b"\0", 1)[0].decode("ascii")
            if re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9.+ _-]{0,15}", value):
                return value
        except (OSError, LauncherError, UnicodeError):
            continue
    return None


@dataclass(frozen=True)
class Installation:
    root: Path
    exefs: Path
    romfs: Path
    ids: dict
    version: str | None

    @property
    def exe(self):
        return self.root / EXE

    @property
    def saves(self):
        return self.root / DATA / "save"


def installation(root, *, template=False):
    root = root.resolve(strict=True)
    for name in (EXE, MODULE):
        file = root / name
        if not file.is_file() or file.stat().st_size == 0:
            raise LauncherError(f"{name} is missing or empty.")
    with (root / EXE).open("rb") as file:
        header = file.read(4)
    if not (header.startswith(b"MZ") if WINDOWS else header == b"\x7fELF" and os.access(root / EXE, os.X_OK)):
        raise LauncherError("The executable does not match this platform or cannot be run.")
    exefs = (root / DATA / "exefs").resolve()
    romfs = (root / DATA / "romfs").resolve()
    ids = build_ids(exefs)
    if not template:
        validate_romfs(romfs)
    return Installation(root, exefs, romfs, ids, display_version(root / DATA))


def detect_installation():
    candidates = (REPO / "games/Orbitals", REPO, REPO / "build/orbitals-linux",
                  REPO / "build/orbitals-debug/orbitals-dev-win64", REPO.parent / "Switch2RECOMP")
    for candidate in candidates:
        try:
            return installation(candidate)
        except (OSError, LauncherError):
            continue
    return None


GAMEPAD_INVENTORY = "Orbitals gamepad inventory v1"


def gamepad_inventory(game):
    """Run the same detection backend as the game without starting an Orbitals session."""
    bundled = "runtime_win64_win32.exe" if WINDOWS else "runtime_linux_x11"
    candidates = ([game.exe] if game else []) + [REPO / "bin" / bundled]
    for path in candidates:
        try:
            with path.open("rb") as binary, mmap.mmap(binary.fileno(), 0, access=mmap.ACCESS_READ) as data:
                # Older binaries treat unknown arguments as a request to start the game.
                if data.find(GAMEPAD_INVENTORY.encode("ascii")) < 0:
                    continue
        except (OSError, ValueError):
            continue
        try:
            env = dict(os.environ)
            env.pop("SWR_GAMEPAD_BACKEND", None)
            result = subprocess.run([str(path), "--list-gamepads"], cwd=path.parent, capture_output=True,
                                    encoding="utf-8", errors="replace", timeout=5, stdin=subprocess.DEVNULL,
                                    env=env,
                                    creationflags=subprocess.CREATE_NO_WINDOW if WINDOWS else 0)
        except (OSError, subprocess.SubprocessError) as err:
            raise LauncherError("Could not check the connected controllers.") from err
        lines = result.stdout.splitlines()
        if (result.returncode or len(lines) < 2 or lines[0] != GAMEPAD_INVENTORY
                or not lines[1].startswith("backend\t")):
            raise LauncherError("The runtime returned an invalid controller list.")
        devices = []
        for line in lines[2:]:
            kind, _, fields = line.partition("\t")
            mapped, _, name = fields.partition("\t")
            if kind == "pad" and mapped in ("0", "1") and name:
                devices.append((name[:96], mapped == "1"))
        return lines[1].split("\t", 1)[1], devices
    raise LauncherError("Controller detection needs an updated Orbitals runtime.")


def local_binary_files(root):
    files = [root / EXE, root / MODULE]
    files.extend(root.glob("*.dll" if WINDOWS else "lib*.so*"))
    return sorted(set(files))


def prepare(source, template, installs, progress):
    """Pair compatible player data with a local native build, publishing only complete installs."""
    exefs = source / "exefs"
    romfs = source / "romfs"
    safe_entry(source)
    exfiles, romfiles = tree_files(exefs), tree_files(romfs)
    validate_romfs(romfs)
    if build_ids(exefs) != template.ids:
        raise LauncherError("This ExeFS version does not match the build. Recompile this version with SwitchRecomp.")
    for name in ("global.utoc", "Orbitals-Switch2.utoc"):
        relative = PAK_DIR / name
        reference = template.romfs / relative
        if reference.is_file():
            with read_local(romfs, relative) as source_file, reference.open("rb") as trusted_file:
                if hashlib.file_digest(source_file, "sha256").digest() != hashlib.file_digest(
                        trusted_file, "sha256").digest():
                    raise LauncherError(f"RomFS version does not match ({name} differs from the reference build).")
    binaries = local_binary_files(template.root)
    total = sum(exfiles.values()) + sum(romfiles.values()) + sum(p.stat().st_size for p in binaries)
    installs.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(installs).free < total + 64 * 1024 * 1024:
        raise LauncherError(f"Not enough disk space. Free at least {(total / 2**30):.1f} GiB and try again.")
    with tempfile.TemporaryDirectory(dir=installs, prefix=".preparing-") as scratch:
        stage = Path(scratch) / "build"
        stage.mkdir()
        complete = 0
        for root, name, files in ((exefs, "exefs", exfiles), (romfs, "romfs", romfiles)):
            copy_files(root, stage / DATA / name, files, lambda done: progress(complete + done, total))
            complete += sum(files.values())
        for binary in binaries:
            shutil.copy2(binary, stage / binary.name)
            complete += binary.stat().st_size
            progress(complete, total)
        ini = template.root / "Orbitals.ini"
        if ini.is_file():
            shutil.copy2(ini, stage / ini.name)
        version = display_version(source)
        if version:
            for relative in (Path("control.nacp"), Path("control/control.nacp")):
                try:
                    with read_local(source, relative) as metadata:
                        content = metadata.read(0x4000)
                    if content[0x3060:0x3070].split(b"\0", 1)[0].decode("ascii") == version:
                        (stage / DATA / "control.nacp").write_bytes(content)
                        break
                except (OSError, LauncherError, UnicodeError):
                    continue
        installation(stage)
        destination = installs / ("Orbitals-" + timestamp())
        stage.rename(destination)
    LOG.info("event=prepare bytes=%s", total)
    return installation(destination)


class Profiles:
    def __init__(self, root):
        self.root = root
        (root / "profiles").mkdir(parents=True, exist_ok=True)
        self.path = root / "launcher.json"
        self.data = (json.loads(self.path.read_text(encoding="utf-8")) if self.path.exists()
                     else {"profiles": {}, "active": None, "install": "", "issues": ISSUES_URL})
        if not self.data["profiles"]:
            self.create("Player")
        for profile_id in self.entries:
            if not re.fullmatch(r"[0-9a-f]{32}", profile_id):
                raise LauncherError("Invalid profile id.")
            path = root / "profiles" / profile_id
            archived = root / "archived-profiles" / profile_id
            if not path.exists() and archived.is_dir():
                archived.rename(path)
            if not (path / "save").exists():
                backups = path / "backups"
                previous = max(backups.glob("BeforeRestore-*"), default=None)
                if previous:
                    previous.rename(path / "save")

    def commit(self, data):
        with atomic_output(self.path) as output:
            output.write_bytes(json_bytes(data))
        self.data = data

    @property
    def active(self):
        return self.data["active"]

    @property
    def entries(self):
        return self.data["profiles"]

    @property
    def profile(self):
        return self.entries[self.active]

    @property
    def directory(self):
        if not re.fullmatch(r"[0-9a-f]{32}", self.active):
            raise LauncherError("Invalid profile id.")
        return self.root / "profiles" / self.active

    @property
    def saves(self):
        return self.directory / "save"

    @property
    def settings(self):
        stored = {key: LEGACY_VALUES.get(value, value) if isinstance(value, str) else value
                  for key, value in self.profile["settings"].items()}
        return DEFAULT_SETTINGS | stored

    def update(self, **changes):
        self.commit(self.data | changes)

    def select(self, profile_id):
        if profile_id not in self.entries:
            raise LauncherError("This profile cannot be found.")
        self.update(active=profile_id)
        LOG.info("event=profile_select")

    def create(self, name):
        name = self.validate_name(name, exclude=None)
        profile_id = uuid4().hex
        (self.root / "profiles" / profile_id / "save").mkdir(parents=True)
        self.update(profiles=self.entries | {profile_id: {"name": name, "settings": {}}}, active=profile_id)
        LOG.info("event=profile_create")

    def validate_name(self, name, *, exclude):
        name = name.strip()
        if not 1 <= len(name) <= 32 or any(ord(c) < 32 for c in name):
            raise LauncherError("Choose a name of 1 to 32 characters.")
        if any(pid != exclude and p["name"].casefold() == name.casefold()
               for pid, p in self.entries.items()):
            raise LauncherError("This profile name is already used.")
        return name

    def update_active(self, **changes):
        self.update(profiles=self.entries | {self.active: self.profile | changes})

    def rename(self, name):
        self.update_active(name=self.validate_name(name, exclude=self.active))
        LOG.info("event=profile_rename")

    def archive(self):
        entries = {pid: profile for pid, profile in self.entries.items() if pid != self.active}
        if not entries:
            raise LauncherError("Keep at least one local profile.")
        directory = self.directory
        destination = self.root / "archived-profiles" / directory.name
        destination.parent.mkdir(exist_ok=True)
        directory.rename(destination)
        try:
            self.update(profiles=entries, active=next(iter(entries)))
        except OSError:
            destination.rename(directory)
            raise
        LOG.info("event=profile_archive")

    def export(self, parent, *, prefix="Orbitals-Save"):
        files = save_files(self.saves)
        if not files:
            raise LauncherError("This profile has no save yet.")
        with tempfile.TemporaryDirectory(dir=parent, prefix=".save-") as scratch:
            stage = Path(scratch) / "save"
            copy_files(self.saves, stage, files)
            destination = parent / (prefix + "-" + timestamp())
            stage.rename(destination)
        LOG.info("event=save_copy files=%s", len(files))
        return destination

    def backup(self):
        parent = self.directory / "backups"
        parent.mkdir(exist_ok=True)
        return self.export(parent, prefix="Backup")

    def restore(self, source):
        files = save_files(source)
        if not files:
            raise LauncherError("No SwitchRecomp save found in this folder.")
        with tempfile.TemporaryDirectory(dir=self.directory, prefix=".restore-") as scratch:
            stage = Path(scratch) / "save"
            copy_files(source, stage, files)
            previous = self.directory / "backups"
            previous.mkdir(exist_ok=True)
            backup = previous / ("BeforeRestore-" + timestamp())
            self.saves.rename(backup)
            try:
                stage.rename(self.saves)
            except OSError:
                backup.rename(self.saves)
                raise
        LOG.info("event=save_restore files=%s", len(files))
        return backup


def save_files(root):
    files = tree_files(root, max_files=4096)
    if any(len(path.parts) < 2 or path.parts[0] not in SAVE_KINDS for path in files):
        raise LauncherError("Select the save folder that holds user, system or device.")
    if sum(files.values()) > 512 * 1024 * 1024:
        raise LauncherError("The save folder is larger than 512 MiB.")
    for path in files:
        if path.name in SAVE_NAMES:
            with read_local(root, path) as stream:
                if stream.read(4) != b"GVAS":
                    raise LauncherError(f"Unreadable Orbitals save ({path.name}).")
    return files


def link_directory(target, source):
    if WINDOWS:
        cmd = Path(os.environ["SystemRoot"]) / "System32/cmd.exe"
        command = 'cmd.exe /D /V:OFF /C mklink /J "%ORBITALS_LINK%" "%ORBITALS_TARGET%"'
        result = subprocess.run(command, executable=cmd, capture_output=True,
                                creationflags=subprocess.CREATE_NO_WINDOW,
                                env=os.environ | {"ORBITALS_LINK": str(target), "ORBITALS_TARGET": str(source)})
        if result.returncode:
            raise LauncherError("Cannot create a local junction. Use an NTFS volume.")
    else:
        target.symlink_to(source, target_is_directory=True)


def shader_tools(game):
    for parent in (game.root, *list(game.root.parents)[:3], REPO):
        for directory in (parent / "tools/nxshader", parent / "nxshader"):
            if (directory / "nxs_live.py").is_file():
                return directory
    return None


def session_directory(game, profiles):
    """Create a fresh executable layout that keeps the runtime's native saves profile-specific."""
    run = profiles.directory / "runs" / timestamp()
    data = run / DATA
    data.mkdir(parents=True)
    try:
        for binary in local_binary_files(game.root):
            target = run / binary.name
            try:
                os.link(binary.resolve(strict=True), target)
            except OSError:
                shutil.copy2(binary, target)
        for name, target in (("exefs", game.exefs), ("romfs", game.romfs), ("save", profiles.saves)):
            link_directory(data / name, target)
        fonts = game.root / DATA / "fonts"
        if fonts.is_dir():
            link_directory(data / "fonts", fonts.resolve())
        tools = shader_tools(game)
        if tools:
            link_directory(run / "nxshader", tools.resolve())
        safe_entry(profiles.saves)
        return run
    except (OSError, LauncherError):
        shutil.rmtree(run)
        raise


def launch_options(game, run, cache, settings):
    validate_settings(settings)
    args = [str(run / EXE), "--renderer=vulkan",
            f"--docked={int(graphics.effective_mode(settings.get('gfx_preset', 'High'), settings['mode']) != 'Portable')}",
            f"--fullscreen={int(settings['display'] == 'Borderless')}",
            f"--keyboard_player={settings['keyboard']}"]
    if settings["mouse_camera"] is not None:
        args.append(f"--mouse_right_stick={int(settings['mouse_camera'])}")
    if settings["mouse_sensitivity"] is not None:
        args.append(f"--mouse_sensitivity={settings['mouse_sensitivity'] / 100:.2f}")
    if WINDOWS and not settings["vibration"]:
        args.append("--vibration=0")
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("SWR_", "NXRT_", "SENTRY_"))}
    env.update(SWR_GPU="nvn", NXRT_GFX="vulkan", SWR_WINDOW_SIZE=settings["size"],
               NXRT_VK_PRESENT="fifo" if settings["vsync"] else "immediate",
               NXRT_VK_UPSCALE=settings["upscale"].lower(),
               SWR_AUDIO_VOLUME=str(settings["volume"]),
               NXRT_SHADER_CACHE=str(cache / "cache"), NXRT_VK_CACHE_DIR=str(cache / "vk"),
               NXRT_VK_WB_GPU=os.environ.get("NXRT_VK_WB_GPU", "1"),   # write-backs compared on the GPU, as measured (STEAM_DECK_PERFORMANCE.md)
               # The flight recorder (the last 120 frames and every draw, for F5 / gamectl captures) is a development tool: ~440 MB of host memory
               # and a frame copy each present. Off for players; NXRT_VK_FLIGHT=1 in the environment keeps it
               NXRT_VK_FLIGHT=os.environ.get("NXRT_VK_FLIGHT", "0"))
    if os.environ.get("ORBITALS_DIAGNOSTICS") == "1":   # the 3D resolution the game draws at in the log (tools/release/bench_report.py)
        env["NXRT_VK_LOG_RES"] = "1"
    seed = pipeline_seed(game)
    if seed:
        env["NXRT_VK_PIPELINE_SEED"] = str(seed)   # pipelines built while the game boots, not at their first draw (tools/nxshader/pipeline_seeds)
    if WINDOWS:
        python = Path(sys.executable).with_name("python.exe")
        if python.is_file():
            env["SWR_PYTHON"] = str(python)
        add_shader_toolchain(game, env)
    else:
        env["SWR_PYTHON"] = sys.executable
        env["LD_LIBRARY_PATH"] = os.pathsep.join(
            path for path in (str(run), str(game.root / "lib"), env.get("LD_LIBRARY_PATH")) if path)
    return args, env


def add_shader_toolchain(game, env):
    """The .NET runtime and SPIRV-Tools the shader translator uses (Windows: the installation's toolchain folder), into env."""
    if not WINDOWS:
        return env
    for parent in (game.root.parent, REPO / "build/orbitals-debug"):
        toolchain = parent / "toolchain"
        dotnet, vulkan = toolchain / "dotnet10", toolchain / "vulkan-bin"
        if dotnet.is_dir():
            env["DOTNET_ROOT"] = str(dotnet)
            env["PATH"] = str(dotnet) + os.pathsep + env.get("PATH", "")
        if vulkan.is_dir():
            env["NXSHADER_TOOLS"] = str(vulkan)
            env["PATH"] = str(vulkan) + os.pathsep + env.get("PATH", "")
        if dotnet.is_dir() or vulkan.is_dir():
            break
    return env


# Shader preparation: every shader of the game translated to SPIR-V from the user's own files before the first session
# (tools/nxshader/nxs_prepare.py), so the first session does not stop to translate them. Vulkan pipelines are still built by the
# driver when a draw first needs one (the runtime keeps them in its pipeline cache).
SHADER_PREP_TIMEOUT = 4 * 3600


def pipeline_seed(game):
    """The Vulkan pipeline seed list of this game build (tools/nxshader/pipeline_seeds/<main build id>.tsv), or None."""
    tools = shader_tools(game)
    if tools is None:
        return None
    seed = tools / "pipeline_seeds" / (game.ids["main"][:32] + ".tsv")
    return seed if seed.is_file() else None


def shader_cache_root(game, profiles):
    return profiles.root / "shader-cache" / game.ids["main"]


def shader_preparation(game, profiles):
    """The summary of the last finished preparation of this build's shader cache (its PREPARED file), or None."""
    path = shader_cache_root(game, profiles) / "cache" / "PREPARED"
    try:
        info = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return info if isinstance(info, dict) and info.get("discovered") else None


def run_tool(task, args, timeout, env):
    """subprocess.run with its output, under a task that can stop it and what it starts."""
    with task.process(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, stdin=subprocess.DEVNULL) as process:
        try:
            out, err = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            task.tree.end()
            raise
    task.check("The shader preparation was stopped.")
    return subprocess.CompletedProcess(args, process.returncode, out, err)


def linux_shader_deps(profiles, tools, env, on_progress=lambda event: None, task=None):
    """Linux (SteamOS included): the .NET runtime, SPIRV-Tools and a C++ compiler for the decoder in the launcher's private folder
    (tools/nxshader/nxs_deps.py: official downloads checked by digest, nothing installed on the system; kept for later runs, which are
    then offline). Returns env with them and the compiler path."""
    result = run_tool(task or Task(), [sys.executable, "-B", str(tools / "nxs_deps.py"), "--dest", str(profiles.root / "tools" / "shader-deps")],
                      3600, env)
    ready = None
    for line in result.stdout.splitlines():
        if line.startswith("{"):
            event = json.loads(line)
            on_progress(event)
            if event.get("phase") == "deps_ready":
                ready = event
            elif event.get("phase") == "error":
                raise LauncherError("The shader tools could not be set up: " + str(event.get("error")))
    if result.returncode or not ready:
        raise LauncherError("The shader tools could not be set up: " + (result.stderr.strip()[-300:] or f"exit {result.returncode}"))
    dotnet = Path(ready["dotnet"]).parent
    env = dict(env, DOTNET_ROOT=str(dotnet), NXSHADER_TOOLS=ready["spirv_tools"],
               PATH=os.pathsep.join((str(dotnet), ready["spirv_tools"], env.get("PATH", ""))))
    return env, ready["cxx"]


def shader_decoder(profiles, tools, env, on_progress=lambda event: None, cxx=None, task=None):
    """The Kraken decoder for the game's shader archives, built once on this PC from ooz's source (tools/nxshader/nxs_kraken.py)."""
    folder = profiles.root / "tools" / "kraken"
    library = folder / ("ooz_kraken.dll" if WINDOWS else "libooz_kraken.so")
    if library.is_file():
        return library
    on_progress({"phase": "decoder", "status": "building"})
    if WINDOWS:
        for gcc in (REPO / "gcc/Library/bin", REPO.parent / "gcc/Library/bin"):
            if (gcc / "x86_64-w64-mingw32-g++.exe").is_file():
                env = dict(env, PATH=str(gcc) + os.pathsep + env.get("PATH", ""))
                break
    result = run_tool(task or Task(), [sys.executable, "-B", str(tools / "nxs_kraken.py"), "--out", str(folder),
                                       *(["--cxx", str(cxx)] if cxx else [])], 600, env)
    lines = [line for line in result.stdout.splitlines() if line.startswith("{")]
    event = json.loads(lines[-1]) if lines else {}
    if result.returncode or event.get("phase") != "built" or not library.is_file():
        raise LauncherError("The shader archive decoder could not be built: " + str(event.get("error") or result.stderr.strip()[-300:]))
    LOG.info("event=shader_decoder_built")
    return library


def prepare_shaders(game, profiles, on_progress=lambda event: None, task=None):
    """Translate every shader of this build's game files into its shader cache. Returns the final summary (nxs_prepare.py "done").
    Stopping `task` keeps what is translated: nxs_prepare.py does the rest the next time."""
    task = task or Task()
    tools = shader_tools(game)
    if tools is None:
        raise LauncherError("The shader translator (nxshader) is missing from this installation.")
    env = add_shader_toolchain(game, {key: value for key, value in os.environ.items() if not key.startswith(("SWR_", "NXRT_"))})
    cxx = None
    if not WINDOWS:
        env, cxx = linux_shader_deps(profiles, tools, env, on_progress, task)
    decoder = shader_decoder(profiles, tools, env, on_progress, cxx, task)
    dotnet = Path(env["DOTNET_ROOT"]) / ("dotnet.exe" if WINDOWS else "dotnet") if env.get("DOTNET_ROOT") else Path("dotnet")
    cache = shader_cache_root(game, profiles)
    cache.mkdir(parents=True, exist_ok=True)
    args = [sys.executable, "-B", str(tools / "nxs_prepare.py"), "--romfs", str(game.romfs), "--cache", str(cache / "cache"),
            "--work", str(profiles.root / "shader-prep" / game.ids["main"]), "--decoder", str(decoder), "--dotnet", str(dotnet)]
    LOG.info("event=shader_preparation_started")
    last = {}
    started = time.monotonic()
    with task.process(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, stdin=subprocess.DEVNULL) as process:
        for line in process.stdout:
            if not line.startswith("{"):
                continue
            try:
                event = json.loads(line)
            except ValueError:
                continue
            last = event
            on_progress(event)
            if event.get("phase") == "translate" and event.get("discovered"):   # all translated: publishing them is not cut short
                task.hold(sum(event.get(part, 0) for part in ("cached", "translated", "failed")) >= event["discovered"])
            if time.monotonic() - started > SHADER_PREP_TIMEOUT:
                task.tree.end()
                break
        error = process.stderr.read()
    task.hold(False)
    if task.stopped and last.get("phase") != "done":
        LOG.info("event=shader_preparation_stopped")
        raise Stopped("The shader preparation was stopped. What is translated is kept: preparing again does the rest.")
    if last.get("phase") != "done":
        LOG.error("event=shader_preparation_failed phase=%s", last.get("phase"))
        raise LauncherError("Shader preparation stopped: " + str(last.get("error") or error.strip()[-300:] or last.get("phase")))
    LOG.info("event=shader_preparation_done discovered=%s translated=%s cached=%s failed=%s seconds=%s", last.get("discovered"),
             last.get("translated"), last.get("cached"), last.get("failed"), last.get("seconds"))
    return last


def validate_settings(settings):
    width, separator, height = settings["size"].partition("x")
    if (not separator or not width.isascii() or not height.isascii() or
            not width.isdecimal() or not height.isdecimal() or len(width) > 4 or len(height) > 4 or
            not (320 <= int(width) <= 8192 and 240 <= int(height) <= 8192)):
        raise LauncherError("Enter a valid window size, for example 1280x720.")
    if (settings["mode"] not in graphics.RENDER_MODES or settings["display"] not in {"Windowed", "Borderless"}
            or settings["upscale"] not in {"Standard", "Sharp", "FSR1"}
            or settings["keyboard"] not in (1, 2) or not 0 <= settings["volume"] <= 100
            or not isinstance(settings["vsync"], bool)
            or not isinstance(settings["vibration"], bool)
            or settings["mouse_camera"] is not None and type(settings["mouse_camera"]) is not bool
            or settings["mouse_sensitivity"] is not None and (
                type(settings["mouse_sensitivity"]) is not int or not 10 <= settings["mouse_sensitivity"] <= 500)
            or settings["fps"] not in graphics.FPS_MODES
            or settings["tv_resolution"] not in graphics.TV_RESOLUTIONS
            or settings["button_layout"] not in BUTTON_LAYOUTS or settings["gyro"] not in GYRO_MODES
            or settings["gyro_button"] not in GYRO_BUTTONS or settings["gyro_button_mode"] not in GYRO_BUTTON_MODES
            or any(type(settings[key]) not in (int, float) or settings[key] not in GYRO_SENSITIVITIES
                   for key in ("gyro_sensitivity_x", "gyro_sensitivity_y"))
            or type(settings["gyro_calibrate"]) is not bool):
        raise LauncherError("A profile setting is invalid.")
    try:
        graphics.validate(graphics_settings(settings))
    except ValueError as err:
        raise LauncherError(f"Invalid graphics setting: {err}") from err


# Update check: the public repository's releases on GitHub ("owner/name"; empty until the repository is published: no request then).
# Only GET https://api.github.com/repos/<owner/name>/releases/latest is made, on demand or at start when the player enables it;
# with auto-update on, the release's archive for this platform is downloaded, checked against the release's SHA256SUMS.txt and
# unpacked beside this kit (download_update); the player's game is then recompiled by the new kit (outdated_release).
UPDATE_REPOSITORY = "Dimi-Sama/OrbitalsPCRecomp"
RELEASES_URL = f"https://github.com/{UPDATE_REPOSITORY}/releases"


def version_tuple(text):
    """Comparable form of "v0.1.0", "0.1.0-rc1": a release candidate sorts before its release (0.1.0-rc1 < 0.1.0-rc2 < 0.1.0)."""
    main, _, pre = text.strip().lstrip("vV").partition("-")
    numbers = re.findall(r"\d+", main)
    base = tuple(int(n) for n in numbers[:3]) + (0,) * (3 - min(3, len(numbers)))
    if not pre:
        return base + (1, 0)
    rc = re.search(r"\d+", pre)
    return base + (0, int(rc[0]) if rc else 0)


def check_update(current=VERSION, repository=None, opener=None):
    """{"latest", "url", "newer", "notes"} of the repository's latest release, or None when no repository is configured.
    Raises LauncherError when GitHub cannot be reached or answers something unexpected."""
    repository = UPDATE_REPOSITORY if repository is None else repository
    if not repository:
        return None
    if not re.fullmatch(r"[\w.-]+/[\w.-]+", repository):
        raise LauncherError("Invalid update repository (expected: owner/name).")
    import urllib.error
    import urllib.request
    request = urllib.request.Request(f"https://api.github.com/repos/{repository}/releases/latest",
                                     headers={"Accept": "application/vnd.github+json", "User-Agent": f"OrbitalsPCRecomp-Launcher/{current}"})
    try:
        with (opener or urllib.request.urlopen)(request, timeout=8) as response:
            data = json.loads(response.read(1 << 20).decode("utf-8"))
        tag, url = str(data["tag_name"]), str(data["html_url"])
    except urllib.error.HTTPError as err:
        err.close()
        if err.code in (401, 403, 404):
            raise LauncherError("GitHub could not show a release to the launcher. Use Open releases and sign in to GitHub for private previews.") from err
        raise LauncherError("Cannot check for updates (GitHub unreachable or unexpected answer).") from err
    except (OSError, ValueError, KeyError, TypeError) as err:
        raise LauncherError("Cannot check for updates (GitHub unreachable or unexpected answer).") from err
    if not url.startswith("https://github.com/"):
        raise LauncherError("Unexpected update answer.")
    LOG.info("event=update_check latest=%s", tag)
    assets = {}
    for asset in data.get("assets") or ():
        try:
            name, link = str(asset["name"]), str(asset["browser_download_url"])
        except (KeyError, TypeError):
            continue
        if link.startswith("https://github.com/"):
            assets[name] = link
    return {"latest": tag, "url": url, "newer": version_tuple(tag) > version_tuple(current), "notes": str(data.get("body") or "")[:2000],
            "assets": assets}


def kit_archive(version, platform=None):
    """The release archive that holds this platform's kit (tools/release/package.py names)."""
    platform = platform or ("windows" if WINDOWS else "linux")
    return f"OrbitalsPCRecomp-{version}-windows-x64.zip" if platform == "windows" else f"OrbitalsPCRecomp-{version}-linux-x64.tar.gz"


def _fetch(url, opener, timeout=30):
    import urllib.request
    return (opener or urllib.request.urlopen)(urllib.request.Request(url, headers={"User-Agent": f"OrbitalsPCRecomp-Launcher/{VERSION}"}),
                                              timeout=timeout)


def download_update(info, parent=None, on_progress=lambda done, total: None, opener=None, platform=None, task=None):
    """Download the new kit of `info` (check_update), check its SHA-256 against the release's SHA256SUMS.txt and unpack it in
    `parent` (default: the folder holding this kit). Returns the new kit's folder; an existing one is reused, nothing is
    overwritten, and this kit, the game and the saves are left as they are."""
    version = info["latest"].lstrip("vV")
    name = kit_archive(version, platform)
    assets = info.get("assets") or {}
    if name not in assets or "SHA256SUMS.txt" not in assets:
        raise LauncherError(f"The release {info['latest']} has no {name} with its SHA256SUMS.txt: download it from the release page.")
    parent = Path(parent or REPO.parent).resolve()
    target = parent / f"OrbitalsPCRecomp-{version}"
    if (target / "launcher" / "__main__.py").is_file():
        return target
    try:
        with _fetch(assets["SHA256SUMS.txt"], opener) as response:
            sums = response.read(1 << 20).decode("utf-8", errors="replace")
    except OSError as err:
        raise LauncherError("Cannot download the update's SHA256SUMS.txt (GitHub unreachable).") from err
    expected = next((line.split()[0].lower() for line in sums.splitlines()
                     if len(line.split()) == 2 and line.split()[1].lstrip("*") == name), None)
    if not expected or not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise LauncherError(f"SHA256SUMS.txt of {info['latest']} does not list {name}.")
    part = parent / (name + ".part")
    digest = hashlib.sha256()
    try:
        with _fetch(assets[name], opener, timeout=60) as response, part.open("wb") as out:
            total = int(response.headers.get("Content-Length") or 0) if getattr(response, "headers", None) else 0
            done = 0
            while chunk := response.read(1 << 20):
                out.write(chunk); digest.update(chunk); done += len(chunk)
                on_progress(done, total)
                if task:
                    task.check("The download was stopped. Nothing was changed.")
    except Stopped:
        part.unlink(missing_ok=True)
        raise
    except OSError as err:
        part.unlink(missing_ok=True)
        raise LauncherError("The update download stopped (network or disk). Nothing was changed.") from err
    if digest.hexdigest() != expected:
        part.unlink(missing_ok=True)
        raise LauncherError("The downloaded update does not match its SHA-256: it was discarded, nothing was changed.")
    staging = parent / (target.name + ".unpacking")
    shutil.rmtree(staging, ignore_errors=True)
    try:
        unpack_kit(part, staging, name)
        inner = [p for p in staging.iterdir()]
        root = inner[0] if len(inner) == 1 and inner[0].is_dir() else staging
        if not (root / "launcher" / "__main__.py").is_file():
            raise LauncherError("The downloaded archive is not an Orbitals PC Recomp kit.")
        root.rename(target)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
        part.unlink(missing_ok=True)
    LOG.info("event=update_unpacked version=%s", version)
    return target


def unpack_kit(archive, dest, name):
    """Unpack without letting a member land outside dest (absolute paths, '..', links)."""
    dest.mkdir(parents=True)
    root = dest.resolve()
    if name.endswith(".zip"):
        import zipfile
        with zipfile.ZipFile(archive) as z:
            for member in z.infolist():
                path = (root / member.filename).resolve()
                if root not in path.parents and path != root:
                    raise LauncherError("The update archive has a file outside its folder: it was discarded.")
            z.extractall(root)
    else:
        import tarfile
        with tarfile.open(archive) as t:
            t.extractall(root, filter="data")


def launch_kit(kit):
    """Start the launcher of another kit (an update just unpacked) in its own folder, with this Python (it has Tk; the Python
    bundled for the recompiler has none)."""
    options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if WINDOWS else {"start_new_session": True}
    subprocess.Popen([sys.executable, "-B", "-m", "launcher"], cwd=kit, **options)


SETUP_STEPS = 5
# Each setup step's share of the installation time (setup-windows.ps1 / build-orbitals-linux.sh): the game code recompile is most of it.
SETUP_WEIGHTS = (0.03, 0.08, 0.05, 0.82, 0.02)


def install_fraction(step, total, text):
    """The installation's progress in [0, 1] from its step and last line: inside the recompile step, switchrecomp.py's
    "compiled N/M" count moves the bar (a step-only bar sat at 70 % for the whole recompile)."""
    weights = SETUP_WEIGHTS if total == len(SETUP_WEIGHTS) else (1 / max(1, total),) * max(1, total)
    step = max(1, min(int(step or 1), len(weights)))
    done = sum(weights[:step - 1])
    m = re.search(r"compiled (\d+)/(\d+)", str(text or ""))
    inside = int(m[1]) / max(1, int(m[2])) if m else 0.0
    return max(0.0, min(1.0, done + weights[step - 1] * min(1.0, inside)))


def install_jobs():
    """Compiler jobs for the game's recompilation: ~0.5 GB of RAM each, half the logical CPUs, at most 6."""
    cpu_count = getattr(os, "process_cpu_count", os.cpu_count)
    jobs = max(1, min(6, (cpu_count() or 4) // 2))
    if WINDOWS:
        try:
            import ctypes

            class MemoryStatus(ctypes.Structure):
                _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32), ("total", ctypes.c_uint64), ("available", ctypes.c_uint64),
                            ("pt", ctypes.c_uint64), ("pa", ctypes.c_uint64), ("vt", ctypes.c_uint64), ("va", ctypes.c_uint64), ("ex", ctypes.c_uint64)]
            m = MemoryStatus(); m.length = ctypes.sizeof(m)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
                jobs = max(1, min(jobs, int(m.available / 2**30) - 4))   # leave 4 GB for the system
        except (OSError, AttributeError):
            pass
    return jobs


# The Linux packages that provide what setup needs, per package manager (tools/build-orbitals-linux.sh)
LINUX_PACKAGES = {
    "apt": ("sudo apt install", {"gcc": "build-essential", "spirv-val": "spirv-tools", "spirv-opt": "spirv-tools", ".NET 10 runtime": "dotnet-runtime-10.0"}),
    "dnf": ("sudo dnf install", {"gcc": "gcc", "spirv-val": "spirv-tools", "spirv-opt": "spirv-tools", ".NET 10 runtime": "dotnet-runtime-10.0"}),
    "pacman": ("sudo pacman -S --needed", {"gcc": "base-devel", "spirv-val": "spirv-tools", "spirv-opt": "spirv-tools", ".NET 10 runtime": "dotnet-runtime"}),
    "zypper": ("sudo zypper install", {"gcc": "gcc", "spirv-val": "spirv-tools", "spirv-opt": "spirv-tools", ".NET 10 runtime": "dotnet-runtime-10.0"}),
}


def linux_family(os_release=Path("/etc/os-release")):
    """The package manager of this distribution from /etc/os-release (ID, ID_LIKE), or None."""
    try:
        text = os_release.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    ids = " ".join(m[1].strip('"').lower() for m in re.finditer(r"(?m)^(?:ID|ID_LIKE)=(.*)$", text))
    for words, family in (("debian ubuntu linuxmint pop elementary zorin", "apt"), ("fedora rhel centos nobara", "dnf"),
                          ("arch manjaro endeavouros cachyos garuda steamos", "pacman"), ("suse opensuse", "zypper")):
        if any(w in ids.split() for w in words.split()):
            return family
    return None


def linux_deps_error(missing, family=None):
    """A LauncherError naming the missing tools and, when the distribution is known, the command that installs them (copied)."""
    family = family if family is not None else linux_family()
    text = f"Orbitals setup needs: {', '.join(missing)}."
    if family in LINUX_PACKAGES:
        cmd, names = LINUX_PACKAGES[family]
        packages = sorted({names.get(m, m) for m in missing})
        command = f"{cmd} {' '.join(packages)}"
        note = ("\n\nOn some distributions .NET 10 comes from Microsoft's repository: "
                "https://learn.microsoft.com/dotnet/core/install/linux") if ".NET 10 runtime" in missing else ""
        return LauncherError(f"{text}\n\nInstall them with (copied to the clipboard):\n{command}{note}\n\nThen choose Install again. "
                             "A Vulkan driver for your GPU is needed too (BUILDING.md).", copy=command)
    return LauncherError(f"{text}\n\nInstall them with your distribution's packages (gcc, spirv-tools, the .NET 10 runtime), "
                         "then choose Install again. See BUILDING.md.")


def linux_install_command(game_data, install_dir):
    script = REPO / "tools/build-orbitals-linux.sh"
    if not script.is_file():
        raise LauncherError("tools/build-orbitals-linux.sh is missing. Extract the full Linux archive.")
    tools = {name: shutil.which(name) for name in ("bash", "gcc", "dotnet", "spirv-val", "spirv-opt")}
    missing = [name for name, path in tools.items() if not path]
    if tools["dotnet"]:
        try:
            dotnet = subprocess.run([tools["dotnet"], "--list-runtimes"], capture_output=True, text=True, timeout=8)
        except (OSError, subprocess.TimeoutExpired) as err:
            raise LauncherError("Cannot check the .NET runtime. Run 'dotnet --list-runtimes' and check BUILDING.md.") from err
        if dotnet.returncode or not re.search(r"(?m)^Microsoft\.NETCore\.App 10\.", dotnet.stdout):
            missing.append(".NET 10 runtime")
    if missing:
        raise linux_deps_error(missing)
    return [tools["bash"], str(script), str(game_data), str(install_dir / "Orbitals"), str(install_jobs())]


SOURCE_NEEDS = ("exefs/main", "exefs/main.npdm", "romfs")


def source_lacks(folder):
    """What a folder lacks to be the one extracted from the player's copy."""
    return [need for need in SOURCE_NEEDS if not (Path(folder) / need).exists()]


def setup_log(install_dir=None):
    return Path(install_dir or REPO / "games").resolve() / "setup.log"


def discard_unfinished(step, install_dir, started):
    """After a stop in the middle of a step: the linker writes these in place, and the setup takes an existing one for a finished one.
    One from before this setup is left (the objects compiled so far are kept in any case)."""
    unfinished = {3: REPO / "bin" / ("runtime_win64_win32.exe" if WINDOWS else "runtime_linux_x11"),
                  4: install_dir / "Orbitals" / MODULE}.get(step)
    for _ in range(50):   # (Windows lets go of an ended process's file a moment later)
        try:
            if unfinished and unfinished.stat().st_mtime >= started - 2:
                unfinished.unlink()
                LOG.info("event=unfinished_discarded file=%s", unfinished.name)
            return
        except FileNotFoundError:
            return
        except PermissionError:
            time.sleep(0.1)
    raise LauncherError(f"{unfinished} is not complete and could not be removed: delete it before installing again.")


def install_from_files(game_data, install_dir=None, on_progress=lambda step, total, text: None, command=None, rebuild=False, task=None):
    """Build from the player's ExeFS and RomFS and record progress in setup.log. `command` supplies a test installer.
    rebuild: recompile the game code even where a compiled one is already there (an update in place). Stopping `task` keeps what is
    compiled; the first step (unpacking the tools) and the last one are not cut short."""
    task = task or Task()
    game_data = Path(game_data).resolve()
    for need in source_lacks(game_data):
        raise LauncherError(f"{need} is missing in {game_data.name}: choose the folder extracted from your copy, with exefs and romfs.")
    install_dir = Path(install_dir or REPO / "games").resolve()
    if command is None:
        if WINDOWS:
            script = REPO / "tools/release/setup-windows.ps1"
            if not script.is_file():
                raise LauncherError("tools/release/setup-windows.ps1 is missing. Extract the full Windows archive.")
            command = [str(Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"),
                       "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                       "-GameData", str(game_data), "-InstallDir", str(install_dir), "-Jobs", str(install_jobs())]
            if rebuild:   # Linux always recompiles (switchrecomp.py keeps the unchanged objects)
                command[-2:-2] = ["-RebuildCode"]
        elif sys.platform.startswith("linux"):
            command = linux_install_command(game_data, install_dir)
        else:
            raise LauncherError("Orbitals setup supports Windows and Linux x86-64.")
    install_dir.mkdir(parents=True, exist_ok=True)
    log_path = setup_log(install_dir)
    LOG.info("event=install_start jobs=%s", command[-1])
    started = time.time()
    step, last = 0, ""
    task.check("The installation was stopped.")
    task.hold(True)
    with log_path.open("w", encoding="utf-8", errors="replace") as log, task.process(
            command, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
            env=dict(os.environ, ORBITALS_PYTHON=sys.executable)) as process:
        for raw in process.stdout:
            line = raw.decode("utf-8", errors="replace").rstrip()
            log.write(line + "\n"); log.flush()
            m = re.match(r"\s*\[(\d)/(\d)\]\s*(.*)", line)
            if m:
                step, last = int(m.group(1)), m.group(3)
                task.hold(step in (1, SETUP_STEPS))
            elif line.strip():
                last = line.strip()[:120]
            on_progress(step, SETUP_STEPS, last)
        code = process.wait()
    task.hold(False)
    if code and task.stopped:
        discard_unfinished(step, install_dir, started)
        LOG.info("event=install_stopped step=%s", step)
        raise Stopped("The installation was stopped. What is compiled is kept: installing again continues from there.")
    if code:
        raise LauncherError(f"The installation failed (code {code}). The details are in {log_path}.")
    LOG.info("event=install_done")
    return installation(install_dir / "Orbitals")


def graphics_settings(settings):
    """The graphics part of a profile's settings (keys gfx_preset and gfx_<option>) as launcher/graphics.py takes it."""
    return {"preset": settings.get("gfx_preset", "High"), "mode": settings["mode"],
            **{key[4:]: value for key, value in settings.items() if key.startswith("gfx_") and key != "gfx_preset"}}


def apply_graphics(run, text, settings):
    """Writes the session's Unreal user config layer (RomFS overlay) and returns Orbitals.ini with the graphics keys."""
    cvars, ini, files = graphics.launch_changes(graphics_settings(settings))
    for relative, body in files.items():
        write_text(run / DATA / "romfs_overlay" / Path(relative), body)
    for key, value in ini.items():
        text = re.sub(rf"(?m)^\s*{re.escape(key)}\s*=.*$", "", text)
        text = text.rstrip() + f"\n{key}={value}\n"
    return text


def launch(game, profiles):
    game = installation(game.root)
    settings = profiles.settings
    cache = profiles.root / "shader-cache" / game.ids["main"]
    validate_settings(settings)
    run = session_directory(game, profiles)
    try:
        args, env = launch_options(game, run, cache, settings)
        seed_cache(game, cache)
        cache.mkdir(parents=True, exist_ok=True)
        ini = game.root / "Orbitals.ini"
        text = ini.read_text(encoding="utf-8-sig", errors="replace") if ini.is_file() else ""
        fps = graphics.FPS_MODES[settings["fps"]]
        keys = ("ue_fps", "display_resolution", "button_layout", "gyro", "gyro_sensitivity_x", "gyro_sensitivity_y", "gyro_button",
                "gyro_button_mode", "gyro_calibrate")
        text = re.sub(rf"(?m)^\s*({'|'.join(keys)})\s*=.*$", "", text)
        tv = graphics.TV_RESOLUTIONS[settings["tv_resolution"]]
        text = text.rstrip() + f"\nue_fps={fps}\n" + (f"display_resolution={tv}\n" if tv else "")
        if layout := BUTTON_LAYOUTS[settings["button_layout"]]:
            text += f"button_layout={layout}\n"
        calibrate = settings["gyro"] != "Off" and settings["gyro_calibrate"]
        if settings["gyro"] != "Off":
            text += (f"gyro={GYRO_MODES[settings['gyro']]}\ngyro_sensitivity_x={settings['gyro_sensitivity_x']:.2f}\n"
                     f"gyro_sensitivity_y={settings['gyro_sensitivity_y']:.2f}\ngyro_button={GYRO_BUTTONS[settings['gyro_button']]}\n"
                     f"gyro_button_mode={GYRO_BUTTON_MODES[settings['gyro_button_mode']]}\n" + ("gyro_calibrate=1\n" if calibrate else ""))
        write_text(run / "Orbitals.ini", apply_graphics(run, text, settings))
        with (run / "console.log").open("wb") as console:
            process = subprocess.Popen(args, cwd=run, env=env, stdout=console, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL)
    except (LauncherError, OSError):
        prune_run_payload(run)
        raise
    if calibrate:   # one launch only
        profiles.update_active(settings=profiles.profile["settings"] | {"gyro_calibrate": False})
    LOG.info("event=play pid=%s args=%s fps=%s", process.pid, " ".join(args[1:]), fps)
    return process, run


def seed_cache(game, cache):
    """Seed each per-build cache from any earlier sessions in the installation folder."""
    own = game.root / "Orbitals_nvn"
    for name in ("cache", "vk"):
        source, target = own / name, cache / name
        if source.is_dir() and not target.exists() and any(source.iterdir()):
            LOG.info("event=cache_seeded part=%s", name)
            shutil.copytree(source, target, ignore=shutil.ignore_patterns("dumps"))


def prune_run_payload(run):
    """Only remove the staged links and binaries, leaving logs and guest-created files."""
    paths = [*local_binary_files(run), run / "nxshader"]
    paths.extend(run / DATA / name for name in ("save", "exefs", "romfs", "fonts"))
    for path in paths:
        try:
            if WINDOWS and path.is_junction():
                path.rmdir()
            elif path.is_symlink() or path.is_file():
                path.unlink()
        except OSError:
            LOG.warning("event=session_cleanup_failed")


def import_pc_settings(run, profiles):
    profile_id = run.parent.parent.name
    profile = profiles.entries.get(profile_id)
    if profile is None or run.parent != profiles.root / "profiles" / profile_id / "runs":
        return
    config = ConfigParser(interpolation=None)
    try:
        with read_local(run, Path("orbitals-pc-settings.ini")) as stream:
            config.read_string(stream.read(4096).decode("ascii"))
        raw = config["Settings"]
    except FileNotFoundError:
        return
    changes = {}
    if raw.get("borderless") in ("0", "1"):
        changes["display"] = "Borderless" if raw["borderless"] == "1" else "Windowed"
    if raw.get("window_height") in ("540", "720", "900", "1080"):
        height = int(raw["window_height"])
        changes["size"] = f"{height * 16 // 9}x{height}"
    if raw.get("vsync") in ("0", "1"):
        changes["vsync"] = raw["vsync"] == "1"
    fps = {str(value): label for label, value in graphics.FPS_MODES.items()}
    if raw.get("fps") in fps:
        changes["fps"] = fps[raw["fps"]]
    if raw.get("scene_height") in ("540", "720", "900", "1080"):
        height = int(raw["scene_height"])
        changes["mode"] = "TV" if height == 1080 else f"{height * 16 // 9}x{height}"
        changes["gfx_preset"] = "Custom"
    if re.fullmatch(r"[0-9]{1,3}", raw.get("volume", "")) and int(raw["volume"]) <= 100:
        changes["volume"] = int(raw["volume"])
    if raw.get("keyboard") in ("1", "2"):
        changes["keyboard"] = int(raw["keyboard"])
    if raw.get("mouse") in ("0", "1"):
        changes["mouse_camera"] = raw["mouse"] == "1"
    sensitivity = raw.get("mouse_sensitivity", "")
    if re.fullmatch(r"[0-9]{2,3}", sensitivity) and 10 <= int(sensitivity) <= 500:
        changes["mouse_sensitivity"] = int(sensitivity)
    if raw.get("vibration") in ("0", "1"):
        changes["vibration"] = raw["vibration"] == "1"
    if changes:
        updated = profile | {"settings": profile["settings"] | changes}
        profiles.update(profiles=profiles.entries | {profile_id: updated})
        LOG.info("event=pc_settings_imported fields=%s", ",".join(changes))


def finish_session(run, code, profiles=None):
    LOG.info("event=exit code=%s", code)
    try:
        with atomic_output(run / "exit.json") as output:
            output.write_bytes(json_bytes({"exit_code": code, "ended": datetime.now().astimezone().isoformat()}))
    except OSError:
        LOG.warning("event=exit_record_failed")
    if profiles is not None:
        try:
            import_pc_settings(run, profiles)
        except (ConfigError, KeyError, LauncherError, OSError, UnicodeError):
            LOG.warning("event=pc_settings_import_failed")
    prune_run_payload(run)


RELEASE_IN_EXE = re.compile(rb"(?<![\w.])(\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?) ([0-9a-f]{7,40}|unknown)\x00")


def game_release(game):
    """The release that compiled this installation's executable ("0.1.5-preview.1"), read from the SWR_RELEASE string the runtime
    embeds (runtime/main.c, switchrecomp.py release_id); None when it cannot be read (a development build says "dev")."""
    try:
        data = game.exe.read_bytes()
    except OSError:
        return None
    match = RELEASE_IN_EXE.search(data)
    return match[1].decode("ascii") if match else None


def outdated_release(game, current=VERSION):
    """The older release this installation was compiled by, or None when it matches this launcher (or cannot be told)."""
    release = game_release(game) if game else None
    if release and version_tuple(release) < version_tuple(current):
        return release
    return None


def game_source(game):
    """The folder with exefs and romfs this installation was made from (its links point there), or None."""
    parent = game.exefs.parent
    if game.romfs.parent == parent and (parent / "exefs/main").is_file() and (parent / "exefs/main.npdm").is_file():
        return parent
    return None


def carry_saves(old, new):
    """Copy the old installation's saves into the new one when it has none yet; the old ones stay where they are."""
    if old is None or new is None or old.saves.resolve() == new.saves.resolve() or not old.saves.is_dir():
        return False
    if new.saves.is_dir() and any(new.saves.iterdir()):
        return False
    shutil.copytree(old.saves, new.saves, dirs_exist_ok=True)
    LOG.info("event=saves_carried from=%s", old.root)
    return True


def latest_run(profiles):
    runs = profiles.directory / "runs"
    return max((p for p in runs.iterdir() if p.is_dir()), default=None) if runs.is_dir() else None


def runtime_info(folders):
    for folder in folders:
        if folder is None:
            continue
        try:
            with read_local(folder, Path("Orbitals.log")) as stream:
                header = stream.read(16384).decode("utf-8", errors="replace")
            match = re.search(r"SwitchRecomp - Orbitals \(runtime ([\w.\-]+), built ([^)]+)\)", header)
            if match:
                info = {"recomp_version": match[1], "recomp_built": match[2]}
                release = re.search(r"\] release ([\w.\- ]+)", header)   # "<VERSION> <commit>" (switchrecomp.py release_id)
                if release:
                    info["release"], _, info["commit"] = release[1].strip().partition(" ")
                device = re.search(r"\[vk\] device: ([^\r\n]+)", header)
                if device:
                    info["vulkan_device"] = device[1].strip()
                return info
        except (OSError, LauncherError):
            continue
    return {}
