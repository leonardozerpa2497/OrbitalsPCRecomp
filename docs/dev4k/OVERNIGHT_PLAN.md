# Overnight plan (owner's request, 2026-11-26) — read before starting the night run

Start only when the owner says so; the PC is then free all night. Branch `display-4k`, dev loop `tools/dev4k/`
(push.sh builds + installs and KILLS a running game; orb.sh launch/press/shot; log.sh). Back up the save before any
scripted play (`backup-runtime-0.1.5\save-*.zip`); never save over his progress (scripted runs only load and stand/walk).

## 1. Performance and stability (goal: 60 stable in co-op at his 1800p/4K)
- Bench first: scripted co-op scene (script_file input), A/B alternated, >=2 runs each, metric = share of frames
  over 17.7 ms + p50/p95 (SotC lesson), plus translation ms/frame and GPU ms/frame from the log.
- Measure what is installed: DEFER on/off, O3/native, the bookkeeping commit (bbc17f3), NXRT_VK_SPLIT_MS=0.
- Then by expected gain: exact barrier at pass begin (NXRT_VK_PASS_BARRIER=0 first to get the ceiling), clear folded
  into the next pass, descriptor keyed cache / batched vkAllocateDescriptorSets / dsl dedup, memory-sync per-page
  packing; profile=1 + tools/claude_laptop/prof_symbolize.py on the translation thread. Keep only what wins outside
  run-to-run spread with no visual regression (contact sheet of captures before/after).
## 2. Visual
- Candidates: TSR/secondary upscale quality at 4K output, sharpening (NXRT_VK_SHARPNESS), scalability groups the
  game ignores vs honours (GRAPHICS_SETTINGS.md), 3D resolution above 1080 (secondary screen percentage > 100?).
  Compare captures; nothing that changes the art direction without his decision.
## 3. Gyro aim (design settled: see the session report)
- Direct camera rotation: APlayerController::AddYawInput 0x320a850 / AddPitchInput 0x320a7c0 (x0 = PC, s0 = value),
  RotationInput doubles at PC+0x530 (pitch) / +0x538 (yaw); find a per-frame per-PC hook point (UpdateRotation).
- SDL3 sensors (optional binds), player space (GyroWiki, relax 1.41), auto calibration when still (OpenRAC thresholds:
  1.5 s window, spread < 0.02 rad/s) + manual calibrate stored per pad, tightening ~3-5 deg/s, no smoothing/accel by
  default, per-sample dt, X/Y sensitivity separate (default ~1.5-2), modes Always / Aim only (left trigger) / Off,
  gyro pause/toggle button, reset on reconnect, FOV scaling if aim zooms, route to the DualSense's player.
- ini keys + launcher row; off by default.
## 4. Button layout mod (Nintendo / Xbox / PlayStation)
- Facts: the port maps by position (SDL south -> Switch B, east -> Switch A; gamepad_win32.c:118), so accept is the
  right button. The game ships full Xbox (T_X_*) and PS5 (T_P5_*) glyph sets and BP_InputData_Xbox/Playstation;
  DefaultGame.ini registers them for Windows (CommonInputPlatformSettings_Windows, bCanChangeGamepadType=True) and
  lists GamepadNames XboxSeries / SonyDualSense; the menu has "Invert Face Bottom / Face Right".
- Try: user Game.ini overlay [CommonInputPlatformSettings_Switch2 CommonInputPlatformSettings] with ControllerData
  Xbox/PS and DefaultGamepadName=XboxSeries|SonyDualSense (find the exact GamepadName in the assets); accept on the
  bottom button via a runtime face swap option or the game's own invert setting (check which also moves the glyphs).
- Launcher row "Button layout" writing the ini/overlay; verify by screenshots of menus and prompts.
## Close
Review packet in the morning: what works, numbers, what was dropped and why, how to roll back.
