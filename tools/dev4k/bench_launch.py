# Starts Orbitals the way the launcher's PLAY does (same settings, a new session folder), from a scheduled task in the
# player's desktop session: pythonw bench_launch.py <kit folder>. Writes the session folder to %TEMP%\orb_bench_run.txt.
import os, sys
from pathlib import Path
kit = Path(sys.argv[1]); sys.path.insert(0, str(kit)); os.chdir(kit)
from launcher.core import Profiles, installation, launch
local = Path(os.environ["LOCALAPPDATA"]) / "OrbitalsLauncher"
process, run = launch(installation(kit / "games" / "Orbitals"), Profiles(local))
Path(os.environ["TEMP"], "orb_bench_run.txt").write_text(str(run))
process.wait()
