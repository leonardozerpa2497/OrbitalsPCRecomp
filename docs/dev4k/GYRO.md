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
  stick ends in; the frame applies it like stick input and the stick keeps working. Nothing is applied while the game window is
  not focused or the PC settings panel is open.
- Gate (`gyro_gate`): UpdateRotation alone proves nothing, because `PlayerTick` (main+0x3201750) calls it also while the controller
  is Inactive without a pawn (the title: this is why the earlier synthetic test turned a title ControlRotation). The turn goes
  through only when, read from the controller before the game's UpdateRotation (offline RE of main): Pawn (+0x2e8) is set and an
  untagged pointer (low bit clear), AcknowledgedPawn (+0x358) is the same pawn, the IgnoreLookInput counter (byte +0x33a; the
  virtual at vtable+0x8d8, main+0x2c9d810, returns it != 0) is 0, the PlayerCameraManager (+0x368) is readable, its
  ViewTarget.Target (+0x340) is the pawn and its PendingViewTarget.Target (+0xc00) is empty or the pawn, and DeltaTime > 0. The
  pawn and view target are compared as pointers, never read; anything unreadable closes the gate. A closed gate still takes the
  waiting turn and drops it, so nothing builds up in menus, cinematics or the title. Conservative: that gameplay has the view
  target equal to the pawn is NOT OBSERVED yet; `gyro_log=1` prints every gate change with the raw pointers.
- Pause: the paused world tick (TickActor main+0x3208eb0, paused path +0x3209130) zeroes RotationInput and usually does not call
  PlayerTick, so UpdateRotation does not run at all (the earlier note "DeltaTime 0" was wrong). A turn nobody took for 100 ms is
  dropped when the next sample arrives, and a turn waiting when UpdateRotation comes back after more than 100 ms is dropped too, so
  nothing jumps after a pause (the second rule removes the one sample the first one leaves). A DeltaTime 0 call closes the gate.
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
- Gate tests (`python tests/test_gyro_gate.py`, C in `tests/test_gyro_gate.c`): the real `orbitals_gyro_pad` and hooked
  UpdateRotation on fake guest controllers with small engine stand-ins. Each closed case (title without a pawn, pending view target
  another actor, IgnoreLookInput, camera manager or controller field unreadable, tagged pawn pointer, pawn not acknowledged,
  viewing another pawn, no camera manager) turns nothing for 1.1 s of a 90 deg/s turn, still runs the game's UpdateRotation, logs
  the closed gate and the dropped turn, and opening it causes no jump; both players turn their own camera at once; no jump after a
  2 s pause or DeltaTime 0 frames; the synthetic test starts only after 3 s of an unbroken open gate and passes, ends
  INCONCLUSIVE (never PASS) when the gate closes mid-test, and FAILs when the engine drops the input. Review cases: the 3 s
  window starts over after an UpdateRotation gap (2.4 s open + 3.2 s paused, and a 128 ms gap) and for a new controller on the
  same player (2.4 s old + 0.8 s new), and is per player (a closed P2 does not hold P1 back); a pause while running or settling
  ends INCONCLUSIVE; a game-thread end landing while the frontend thread is inside a synthetic sample (test_lock let go; a clock
  hook in the test places it deterministically, first sample or a later one) stays INCONCLUSIVE, stops the generator there and
  its turn never reaches the camera, nor a new controller's, also when it lands in the last due sample (the loop exits without
  another state check); a turn published just before an end is dropped by that end. A synthetic sample is counted and published
  to the waiting turn in one test_lock section, only while the test still runs on its slot, and every INCONCLUSIVE end clears
  that slot's waiting turn under the same lock (lock order test_lock -> Acc; no clock read under either lock). The earlier build
  fails 15 checks of these. Mutations (gate always
  open, closed turn kept, no seen reset, no gap drop, each check removed, no INCONCLUSIVE, no relock check, no window reset on a
  gap or a new controller, no abort on a gap, no drop of the ended test's turn, no running check at publication, no clear at an
  INCONCLUSIVE end) each fail at least one test.
- Synthetic test and the gate: it starts only after 3 s of continuous open gate on one player's controller (the 3 s start over
  whenever it closes, UpdateRotation stops for over 100 ms or another controller takes the player); once it runs, any such break
  logs `gyro test: INCONCLUSIVE (...)`, gives no result, drops the waiting synthetic turn and cannot be resumed.
- Real pad still 60 s (`gyro_log=1`): resumed after the owner connected the pad; logs and final measured drift are recorded
  in the run report. Initial run exposed the clock rollback above; its uncalibrated P1 zero-drift lines are not a pass.
- Feel: NOT OBSERVED until the owner plays. Values chosen: sensitivity 1.5 x / 1.5 y, tightening 3 deg/s, touchpad hold to pause.

## Not verified
- The gate in the real game: NOT OBSERVED. That it opens in gameplay (view target = pawn) and stays closed at the title, menus and
  cinematics must be checked on the PC with `gyro_log=1` (`gyro gate:` lines) and the synthetic test in gameplay.
- Sign of the axes with a real pad (left turn -> camera left, tilt up -> look up) follows SDL's sensor axes; `gyro_invert` fixes it
  if it is the other way.
- Steam Input / DS5Bridge can take the pad's sensors before SDL; the log line `gyro: P1 ... has a gyro` confirms SDL sees them.
