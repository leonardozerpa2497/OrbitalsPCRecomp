# Gyro aiming (DualSense and any SDL3 pad with motion sensors)

Code: `runtime/orbitals_input.c` (gyro section), `runtime/gamepad_win32.c` (SDL3 sensors). Off by default.

## Follow-up: uneven motion reported over Bluetooth

The owner reported visible stepping with the DualSense Edge over Bluetooth after the night run. Real feel is therefore
**not accepted**; the integral checks below prove total rotation, not even delivery between frames.
The user's run `20261126-134628-2937aefa` used Always, sensitivity 1.5/1.5, and had a 60 presents/s interval near its end.
Its motion diagnostics were off, so the live sensor-to-camera delivery pattern is not established by that log.

Bounded fix brief: reduce sensor batching caused by the independent 60 Hz frontend and game clocks; keep the existing
per-sample integration, calibration, camera gates and sensitivity; run SDL on its existing thread; preserve the 60 Hz
HID/vsync/housekeeping cadence; verify scheduler deadlines and gyro regressions, then Windows build/boot and owner feel.
`motion_wait_until` services motion every 2 ms during the frontend's existing sleep, only when gyro sensors are needed.
Extra polls do not publish HID, run scripts or drive settings; focus is checked again and the latest panel suppression is kept.
It adds no interpolation or smoothing delay. Long frontend work and Bluetooth/device batching can still cause gaps.
The deterministic independent-clock test reproduces empty camera frames at the old delivery cadence and rejects them with
the new scheduler; this is a model, not a claim that Bluetooth hardware has been measured at 500 Hz.
Rollback: Gyro Off with calibration/test/log diagnostics also off disables the extra polling; revert the follow-up commits
to restore the old delivery cadence with gyro on. The real-pad L+R+Plus settings chord suppresses intermediate batches before
the next normal poll opens the panel. The headless backend has a no-op motion poll.

**Live diagnosis after the cadence patch:** the isolated title run `20261126-135538-6c4ec35e` still reports only
21-26 gyro samples per sensor second for P1 (Edge); P2 (ordinary DualSense) reports about 450. The game exits0 and
the launcher state file stays byte-identical. Thus the cadence patch is NOT a demonstrated fix for this complaint.
A standalone SDL3.4.16 probe, after the owner connected USB, measures two Edge endpoints: approximately73 and1001 events/s
over10 host seconds. Windows PnP ancestry identifies the slow endpoint as `DS5BRIDGE0001` under the USBip emulated host;
the fast endpoint is the physical USB controller under the AMD host. DS5Bridge's own log independently reports23-25Hz
before the cable connection and around70-78Hz afterwards. The four extra bridge processes are watchdogs of one main process,
not evidence of competing bridge instances. No bridge, HidHide, driver or network settings were changed.
The reviewed cadence patch is committed locally but is not installed as the owner's runtime; the prior installed runtime
and source files are restored pending a direct-Bluetooth/bridge isolation test. Do not mark real feel accepted.

**Authorized direct Bluetooth isolation:** after the owner's approval, backed up the bridge task, process arguments,
restore file and HidHide state through the documented read IOCTLs. Clean desktop-session `ds5bridge --quit` detached its
virtual device and removed its own hidden-device entry. No manual HidHide write was needed. After unplugging USB the
owner pressed PS to reconnect. A standalone SDL probe measured the real Edge at83.4 events/s over10.002 host seconds,
sensor interval p50 14.288 / p95 23.297 / max37.584ms. The ordinary DualSense measured498.1 events/s in the same probe.
This improves on the bridge path but remains irregular: the bridge is not proven to be the sole cause. USB direct was
about1001 events/s. Evidence `/tmp/orbitals-direct-bluetooth-rate.txt`. Owner feel with the bridge temporarily stopped
is pending; restore the original running DS5Bridge task after that interactive test. Original HidHide snapshot is
`C:\llm\tmp\orb\bridge-test-20261126-140145\hidhide-state.json`; the task was enabled and running.

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
| `gyro_test_delay` | with `gyro_test=1` only: seconds from arming (the runtime's install, monotonic clock) before the test may start; 0-600, an invalid value keeps the previous one; ini only, not in the launcher | 0 |
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
**PC validation, 08:44:** the earlier gate (view target == `AController::Pawn`) was disproved: it opened briefly during the title
world's initialization and closed in the actual co-op ship scene, where each player's view target is a separate controlled actor.
Its synthetic result at 18-20 seconds belonged to initialization, not gameplay. It is replaced by the game-specific gate below.

**PC validation, 09:24 run `controls-native-gate-ps`:** title/menu controllers stay closed by type; both ship roles open at91.34s,
after the intro cinematic closes the gate through IgnoreLookInput. The synthetic test starts on ControllerId1 at94.35s and passes
at96.69s: yaw −134.460° and pitch45.000°, both0.00% error. The co-op screenshot at110s visibly shows the second player's changed
view while the first player's view remains directed toward the wreck. At126.92s P1 closes for shared pause; P2 has no UpdateRotation
for15050ms. At141.97s gameplay resumes, and the resumed screenshot shows no accumulated-turn jump. Evidence: the run's Orbitals.log
under `/tmp/claude-1000/bench/` and `controls-native-gate-ps-{menu,coop,paused,resumed}_s.png` under `/tmp/claude-1000/orb/`.

**PC validation,10:42 run `gyro-native-player1-delayed`:** the runtime arms at0.19s with delay110. Player1's test starts
at110.20s on ControllerId0 and finishes at112.53s: yaw−134.460°, pitch45.000°, applied and measured camera rotation agree
with both integrals to0.00%. The105s/115s screenshots (`-camera-before_s.png`/`-camera-after_s.png`) visibly show only
player1's view turning; player2 keeps looking toward the wreck. This supplies the missing player1 gameplay proof without
relaxing the continuity or terminal-abort rules. Build log `/tmp/orbitals-after-flight-1026.log`: NVN/runtime exit0.

**On-foot pause check, run20261126-121131-332d70f3:** both players leave their seats by holding the right face button;
the screenshots show their characters inside the ship. At264.74s player1 closes for shared pause with controlledactor0,
ViewTarget=MainCharacter; at280.49s it reopens with the same relationship and pause0. Player2's UpdateRotation pauses.
The diagnostic logs only changes of controller or gate reason, so a ship-to-foot transition that stays open does not
produce its own line. This is a gate/pause observation, not a synthetic on-foot camera-result claim.

**Final on-foot synthetic check, run20261126-121733-b70975c9:** final runtime with the ineffective controller-art
probe reverted and the prior two-route code module restored. Both players leave their seats and jump with the bottom
button. With delay170, ControllerId0 starts at170.42s and reports at172.73s: yaw−134.460°, pitch45.000°, both0.00% error.
`final-ps-camera-before_s.png` and `final-ps-camera-after_s.png` visibly show only P1's view rotate inside the ship;
P2's view remains pointed toward the door. Evidence: `/tmp/claude-1000/bench/final-ps-onfoot/Orbitals.log`.
This functional run is not a performance comparison: jumping, exiting seats and the synthetic camera turn change the scene.
- Native: the game's own `APlayerController::UpdateRotation` (main+0x31fdf90, and the game's override main+0x2cea090) is hooked;
  before it runs, the gyro turn is given to `AddYawInput` / `AddPitchInput` (main+0x320a850 / +0x320a7c0), the same functions the
  stick ends in; the frame applies it like stick input and the stick keeps working. Nothing is applied while the game window is
  not focused or the PC settings panel is open.
- Gate (`gyro_gate`): UpdateRotation alone proves nothing, because `PlayerTick` (main+0x3201750) calls it also while the controller
  is Inactive (the title and menus). The turn goes through only when, read before the game's UpdateRotation (offsets from offline RE
  of main, checked against live snapshots of the title, co-op ship gameplay, the pause menu and after resuming):
  - Type first: the controller is an `OrbBasePlayerController` (class singleton main+0x9cb95d0). The check is the engine's own IsA
    (main+0x451d700): the object's class (+0x10) has a base chain array (+0x30) and a signed depth (+0x38); the entry at the wanted
    class's depth must be the wanted class's chain struct, wanted class + 0x30 (not the class pointer). The wanted class's own chain
    must name itself at its depth, and so must the object's class at its own depth before it counts as a well-formed other class
    (the chain is never read past the object's own depth); depths must be 0..256 and the wanted depth not above the actual one; every pointer is checked
    (set, 8-aligned, below 2^48, readable), and every object's base is checked before a field offset is added to it, so a null or
    wrapping pointer never reaches readable memory through its offset. Singletons are only read, never initialized (no StaticClass call): an empty one closes
    the gate. The title's `OrbFrontEndPlayerController` is only 0x750 bytes, so none of the game fields below is read until this holds.
  - Pawn (+0x2e8) set and a plain pointer, AcknowledgedPawn (+0x358) the same, IgnoreLookInput (byte +0x33a) 0, cinematic mode bit
    (+0x4c0 mask 0x02; measured 0x09 / 0x109 in gameplay) clear, MainCharacter (+0x7b8) set.
  - The controlled actor as the game's GetControlledActor (main+0x45144c0) gives it: ControlledActor (+0x7c8) if set (a ship
    station in co-op, a different class per role), else MainCharacter (on foot). A ControlledActor other than MainCharacter must be
    an `OrbControllableActor` (main+0x9cba540) before its own field is read, and its controlling player controller (+0x2f0,
    measured equal to the controller for both players) must be this controller.
  - The PlayerCameraManager (+0x368) is readable, its ViewTarget.Target (+0x340) is that controlled actor and its
    PendingViewTarget.Target (+0xc00) is empty or the same actor.
  - The shared pause is off: the controller's Outer chain (+0x20, at most 32 steps, a repeated object closes, an Outer whose class
    metadata is invalid closes rather than being walked past as "not a level") to its `ULevel`
    (main+0x9c51c88), the level's OwningWorld (+0xc0), the world's GameState (+0x1d8), which must be an `OBGameStateBase`
    (main+0x9cb7bf0) whose pause byte (+0x42b) is 0 (measured 1 while paused and 0 after resuming, for both players). The pause menu
    pointer (+0x880) is not used: only the player who paused has one, and it stays cached after resuming.
  - DeltaTime > 0.
  Anything unreadable, malformed or of another type closes it. A closed gate still takes the waiting turn and drops it.
  `gyro_log=1` prints every gate change with the reason and the raw pointers (pawn, main character, controlled actor and its
  controller, view target, pending, cinematic byte, game state and its pause).
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
  earlier build calibrated 0.24 s after the events stopped). The earlier build fails five of them. Linux and Windows: 8/8 pass.
  The gyro gate C harness models the measured relationship (class metadata, controllers, stations, world and game state) but cannot
  prove the real game's camera turns.
- Gate tests (`python tests/test_gyro_gate.py`, C in `tests/test_gyro_gate.c`): the real `orbitals_gyro_pad` and hooked
  UpdateRotation on fake guest controllers with small engine stand-ins and the engine's class metadata layout (class chains built
  as the game has them; the default scene is co-op at the ship, each player on its own station class). Each closed case turns
  nothing for 1.1 s of a 90 deg/s turn, closes for its own logged reason, still runs the game's UpdateRotation, logs the dropped
  turn, and opening it causes no jump: title without a pawn, the title's frontend controller with a viewed pawn (no read past its
  0x750 bytes), an engine player controller, pending view target another actor, IgnoreLookInput, camera manager or controller field
  unreadable, tagged pawn pointer, pawn not acknowledged, viewing the other player's station, a station viewing the pawn (the
  disproved rule), on foot viewing a station, no main character, a station naming another or no controller, controlling the other
  player's station, a controlled actor that is not controllable (its controller field not read) or unreadable, the cinematic bit,
  a cutscene camera as view target, the shared pause; malformed metadata (negative, 0x80000000 and over-cap depths on either class,
  a chain entry naming the class instead of its chain, chain null / misaligned / unreadable, a tagged class pointer, an empty or
  stale singleton, a malformed game state class); the world chain (no, unreadable or misaligned Outer, an Outer cycle, 40 Outers,
  an unreadable one mid-chain, an Outer whose class is unreadable, tagged or malformed (with a world field, so neither walking
  past it nor taking it as the level opens), a null or wrapping world, game state, Outer or controlled actor pointer whose field
  offset lands on readable memory, an Outer class over the depth cap or negative (its own entry naming it), a shallow (UObject)
  Outer whose own chain entry is unreadable, out of memory or names another class, no level, no world, no or unreadable game state, an engine or other game state class, an unreadable
  pause byte). Both ship roles and on foot open for both players; a shared pause closes both players and both reopen without a
  jump while P1's pause menu pointer stays cached; a shared pause during the synthetic test ends it INCONCLUSIVE; both players turn
  their own camera at once; no jump after a
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
  The runner also builds 22 mutations of the new gate (no own-depth self check of the object's class, an invalid Outer walked past, a field base not checked, no controller type check, the type check after the game fields, the chain
  entry compared with the class, no wanted/actual depth comparison, no negative depth check together with the 2^32 field offset bound (each alone is covered by the other), no cap check, no self check of the
  singleton, view target or pending compared with the pawn, no MainCharacter fallback, no controllable type, owner, main
  character, cinematic or pause check, an unknown game state accepted, no game state type check, no Outer bound, the first Outer
  taken as the level) and requires each to fail. Removing the cycle check alone is not observable (the 32 bound still closes), nor
  is weakening the pointer alignment check (every read also requires alignment).
- Synthetic test and the gate: it starts only after 3 s of continuous open gate on one player's controller (the 3 s start over
  whenever it closes, UpdateRotation stops for over 100 ms or another controller takes the player); once it runs, any such break
  logs `gyro test: INCONCLUSIVE (...)`, gives no result, drops the waiting synthetic turn and cannot be resumed.
  With `gyro_test_delay=N` it also cannot start before N s after arming (logged as `synthetic test armed (start delay N s)`;
  the start line says how long after arming it began); both conditions must hold, the 3 s window keeps its own rules and the
  delay changes nothing else (real gyro, slots, sensitivity, sample path, ends). It exists for player 1: with `gyro=always` P1's
  gate opened during loading (74.05 s), the test started at 77.05 s and correctly ended INCONCLUSIVE at 77.52 s on a 205 ms
  UpdateRotation hitch, before the ship scene (91.96 s); a delay past loading lets the test run in gameplay. Gate tests: no start
  before the delay although the 3 s gate is met and after a 208 ms hitch, start at exactly the delay then PASS; past the delay
  the 3 s window still applies and a gap ends a running delayed test INCONCLUSIVE with no second start; the delay counts from
  arming, not from when the gate opened; the parser keeps 0-600 and refuses -1, 600.5, nan, inf, 1e9, empty text and trailing
  non-whitespace text (invalid input retains the previous delay). Mutations: no delay check,
  delay counted from the clock origin, setting ignored, no bounds, no 3 s window before the start; each fails.
- Real pad still 60 s (`gyro_log=1`): resumed after the owner connected the pad; logs and final measured drift are recorded
  in the run report. Initial run exposed the clock rollback above; its uncalibrated P1 zero-drift lines are not a pass.
- Feel: NOT OBSERVED until the owner plays. Values chosen: sensitivity 1.5 x / 1.5 y, tightening 3 deg/s, touchpad hold to pause.

## Not verified
- Player-one run `gyro-native-player1` did not produce a valid synthetic result: with Always mode selecting slot0, it started
  at77.05s during the initial gameplay/loading transition and ended INCONCLUSIVE at77.52s after a205ms UpdateRotation gap.
  The ship gate opened at91.96s. No P1 PASS or camera-motion proof is claimed from that run's later screenshots. The separate
  delayed run above supplies that proof; the initial aborted result remains INCONCLUSIVE.
- Still to observe on the PC: on-foot gameplay, an additional cinematic beyond the intro and physical-pad directional feel.
  Title/menu rejection, both ship gates, separate synthetic motion for both players and pause/resume
  have been observed as recorded above. The test was armed at startup but waited for continuous gameplay; no initialization PASS
  is counted as acceptance.
- Sign of the axes with a real pad (left turn -> camera left, tilt up -> look up) follows SDL's sensor axes; `gyro_invert` fixes it
  if it is the other way.
- Steam Input / DS5Bridge can take the pad's sensors before SDL; the log line `gyro: P1 ... has a gyro` confirms SDL sees them.

### Interactive sensitivity and touchpad check

The owner reports smooth motion from the regular DualSense over Bluetooth and from the Edge over direct USB; Edge Bluetooth remains jerky. This is observed feel, not acceptance of the uninstalled frontend polling candidate.

The active Player profile now explicitly stores sensitivity 1.0 on both axes (previously the 1.5 defaults). The launcher was closed normally, updated through `Profiles.update_active`, and reopened. A full JSON comparison confirmed that only those two settings changed. Backup: `%LOCALAPPDATA%/OrbitalsLauncher/launcher-before-gyro-speed-20261126-141812.json`; restore those two previous settings to revert. The pause action remains the default Hold to pause.

The owner reported that holding the touchpad did not pause motion. A direct SDL probe subsequently recorded three Edge touchpad presses as button 20, including sustained presses; both controllers' SDL mappings expose `touchpad:b11` (raw joystick button, mapped to gamepad button 20). An isolated harness using the real `orbitals_gyro_pad` and simulated engine checked hold suppression, release/resume and toggle with zero failures. Its first attempt lacked the fake AddInput callbacks and crashed; after correcting that harness setup it passed. This does not reproduce or resolve the reported in-game failure. The owner subsequently confirmed in-game that the Edge over direct USB stops the camera while the touchpad is held, with sensitivity 1.0. Direct-USB hold-to-pause is now observed working; the cause of the earlier failure is not established. DS5Bridge remains temporarily stopped; restoration is still outstanding and must not interrupt the owner's current direct-USB play.
