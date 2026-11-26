# Gyro aiming (DualSense and any SDL3 pad with motion sensors)

Code: `runtime/orbitals_input.c` (gyro section), `runtime/gamepad_win32.c` (SDL3 sensors). Off by default.

## Settings (Orbitals.ini, launcher Controls page)
| key | values | default |
|---|---|---|
| `gyro` | off / always / aim (while the left trigger is held) | off |
| `gyro_sensitivity_x`, `gyro_sensitivity_y` | camera degrees per degree the pad turns (0.5-4 in the launcher) | 1.5 / 1.5 |
| `gyro_button` | touchpad, mute, l3, r3, paddle_left, paddle_right, none | touchpad |
| `gyro_button_mode` | pause (held = gyro off) / toggle | pause |
| `gyro_tightening` | slow turns under this many deg/s are softened (hand shake) | 3 |
| `gyro_invert` | 0, 1 horizontal, 2 vertical, 3 both | 0 |
| `gyro_calibrate` | 1 = calibrate at the next 2 s with the pad lying still, stored per controller model, then back to 0 | 0 |
| `gyro_test` | 1 = synthetic test (below) | 0 |
| `gyro_log` | 1 = once a second per pad: raw mean / spread, bias, drift since calibration | 0 |

Holding the gyro button 3 s with the pad lying still also calibrates and stores it in `gyro_calibration.txt` beside the save
directory, never inside it: the runtime resolves `Orbitals_data/save` (the launcher's link to the active profile's `save`) to the
real directory and writes in its parent, the profile directory (a standalone install: `Orbitals_data`). The save directory keeps
only the game's save kinds, so the launcher's export, backup, problem report and restore keep working, and a restore (which swaps
the save directory) keeps the calibration. It survives a new session and stays isolated from other profiles. If the link cannot be
resolved, the path would be truncated, the resolved directory is not named `save`, or its parent is a volume root, nothing is read
or written. Storage replacement is atomic; failed writes keep the previous file and log that the new calibration is active only
for the current session. Calibration is per controller model (vendor:product), not per physical pad.
A `save/gyro_calibration.txt` written by the earlier build is not read or moved; it makes the launcher refuse that save folder
until it is moved out.

## How it turns the camera
- Native: the game's own `APlayerController::UpdateRotation` (main+0x31fdf90, and the game's override main+0x2cea090) is hooked;
  before it runs, the gyro turn is given to `AddYawInput` / `AddPitchInput` (main+0x320a850 / +0x320a7c0), the same functions the
  stick ends in. They respect IgnoreLookInput (cutscenes) and the frame applies it like stick input; the stick keeps working.
  Nothing is applied while the game is paused (DeltaTime 0), the game window is not focused or the PC settings panel is open.
- Per player: pad slot n turns the player controller whose LocalPlayer ControllerId is n (player 1 = 0; logged as
  `gyro: player controller ... has ControllerId n`). Every pad with a gyro aims its own player (both DualSenses in co-op).
- Samples: every SDL sensor event with its own timestamp (frame-rate independent); player-space yaw (GyroWiki, relax 1.41) from
  the accelerometer's gravity; tightening; auto calibration when the pad lies still 1.5 s (every axis within 0.02 rad/s and under
  0.04 rad/s; it never moves a stored calibration by more than 0.015 rad/s). The still window restarts on any sample that goes back
  in time or comes over 50 ms after the previous one, so a switch between the sensor clock and the host poll clock (either way)
  never counts as time lying still.

## Tests
- Synthetic (`gyro_test=1`, measured 2026-11-26 on the PC): 1 s of 90 deg/s left turn then 1 s of 30 deg/s upward tilt injected
  through the same sample path at 250 Hz, sensitivity 1.5: yaw integral x sensitivity -134.460 deg, given to the engine -134.460,
  camera ControlRotation turned -134.460 (0.00 %); pitch 45.000 / 45.000 / 45.000 (0.00 %) -> PASS (limit 2 %).
- Storage regression tests (`python tests/test_gyro_calibration.py`): eight tests run the runtime's actual C functions inside the
  launcher's own contract (`Profiles`, `session_directory` and its links, `save_files`, `report_saves`, export, backup, restore):
  fresh sessions, profile isolation, preserving another controller model, failed-write retention, saves still exportable with
  calibration stored, restore keeping it (also for a session started before the restore), a standalone install, an unexpected or
  dangling save link storing nothing, the host-clock to sensor-clock rollback, and the forward jump when polling takes over (the
  earlier build calibrated 0.24 s after the events stopped). The earlier build fails five of them. Linux: 8/8 pass; Windows: run by
  the PC gate.
- Real pad still 60 s (`gyro_log=1`): resumed after the owner connected the pad; logs and final measured drift are recorded
  in the run report. Initial run exposed the clock rollback above; its uncalibrated P1 zero-drift lines are not a pass.
- Feel: NOT OBSERVED until the owner plays. Values chosen: sensitivity 1.5 x / 1.5 y, tightening 3 deg/s, touchpad hold to pause.

## Not verified
- In menus the main menu's player controller also runs UpdateRotation (the synthetic test turned its ControlRotation): the menu
  uses its own camera, so no visible effect is expected, but this was not observed with a real pad.
- Sign of the axes with a real pad (left turn -> camera left, tilt up -> look up) follows SDL's sensor axes; `gyro_invert` fixes it
  if it is the other way.
- Steam Input / DS5Bridge can take the pad's sensors before SDL; the log line `gyro: P1 ... has a gyro` confirms SDL sees them.
