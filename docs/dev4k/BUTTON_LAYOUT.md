# Button layout (Nintendo / Xbox / PlayStation) — what works and why

Ini keys (Orbitals.ini, launcher Controls page): `button_layout=nintendo|xbox|playstation`, `button_accept_bottom=0|1`.
Code: `runtime/orbitals_input.c` (button layout section). Measured on the gaming PC on 2026-11-26.

## Icons (PlayStation and Xbox observed; paired performance gate completed)
The regenerated module and reviewed runtime were installed at 09:24. Run `controls-native-gate-ps` shows Cross for Select in the
main menu, R2 for both players' co-op prompts, Square for the second player's action and Circle for Back in pause. The bounded
trace records actual replacement textures from caller main+40c4ed4. Screenshots are in `/tmp/claude-1000/orb/controls-native-gate-ps-*_s.png`.
PlayStation Settings shows Cross/Circle in `ps-settings-back_s.png`; `ps-main-after-back_s.png` shows the return to the main menu.
Run `controls-native-gate-xbox` shows RT for both co-op players and X for player two's action. Player two opens pause,
returns with the right face button (B), then opens pause again and selects Resume with the bottom face button (A).
The corresponding `-coop`, `-paused`, `-back-result` and `-resumed` screenshots were inspected.
`xbox-settings-clean_s.png` is actually the main menu with A Select: the timed navigation ran during a transition.
It is not Settings evidence. The later manually inspected `xbox-settings-verified_s.png` shows A Select/B Back in Settings;
P1's bottom button entered it and right button returned to the main menu. P2's right button did not navigate this front-end screen
before joining co-op; P2's in-game pause/back/resume flow did work. Do not count the front-end P2 attempt as a pass.
Run `xbox-player2-settings` subsequently verifies P2's own in-game Settings flow: Plus opens pause, D-pad down selects
Settings, bottom A enters, and right B returns to pause. Inspected `-settings-selected`, `-settings-native`, `-settings`
and `-pause-after-settings` screenshots show A Select/B Back. P1 closes the additional native PC panel before the P2
game-Back check; that separate panel action is not counted as P2 evidence.
The eight-run Nintendo/PlayStation comparison and installed
Windows launcher evidence are recorded in [LAYOUT_BENCH.md](LAYOUT_BENCH.md); no consistent FPS regression was measured.

Before this fix, the 08:30-08:44 PC runs with PlayStation selected showed Nintendo B/A in title, main menu, lobby and settings, and ZR/Y in co-op.
The regenerated module exports the dynamic lookup and both generated caller chunks use `CALLDYN`, but the opt-in
`SWR_ICON_TRACE=1` logs no calls to the wrapper. The installation log alone is not proof that a prompt reaches it.
- Root cause (from `main.bin`, 2026-11-26): those prompts never call the icon lookup main+0x40c5ce0. They go through the action
  prompt main+0x40c5020 `(UInputAction*, asset, UTexture2D** pad, UTexture2D** keyboard, bool* label, FKey* labelKey, player) -> bool`,
  which gets the action's mapped keys (0x40cb140) and looks each one up in `InputIcons` itself, inlined, with the platform hard-coded:
  a gamepad key asks Switch 3 (`and w10, w10, #3` at 0x40c51ec, `cmp w8, #3` at 0x40c520c) and writes `*pad`; a keyboard key asks
  Desktop 0 and writes `*keyboard`, or, without one, the asset's default texture (+0x80), `*label = true` and a copy of that keyboard
  key into `*labelKey` (so x5 is never the gamepad key). Its only references are two direct `bl` (0x40c4ed0 in the input-action
  widget, 0x40d3990), both zeroing `*pad` first.
- Fix (now built and observed on the PC): `codegen.py` `DYNAMIC_CALLS` now lists 0x40c5020 too, and the runtime wraps it
  (`icon_prompt`, runtime/orbitals_buttons.h; install with its own fail-closed word checks, `icon_prompt_install`). Before the call,
  while the caller holds the asset (exactly the original's own lifetime for it), it lists for every live `InputIcons` key (allocation
  bits) its Switch texture and that key's PlayStation / Xbox texture, through each key's platform-map hash as the game does. After the
  call it reads only `*pad`: if the call changed it to a Switch texture of that list, it writes the same key's layout texture. Kept
  Switch (fail closed): a Switch texture shared by keys whose replacements differ, a missing or NULL replacement, an unchanged slot, or
  a map (the key map or any key's platform map) that fails any bound: count <= capacity <= limit, free count <= count, flag bit count ==
  count <= flag capacity (<= 128 without a heap pointer), set flag bits == count - free count, alignment / readability, hash size, index
  range, chain longer than the map, or a hash chain reaching an element whose flag is clear (freed). Registers, return value, `*keyboard`, `*label`, `*labelKey` and its reference count are the
  original's. Logs: "button prompts: PlayStation icons (action prompt main+40c5020 wrapped)" at start; once, "a prompt keeps its Switch
  icon (reason)". `SWR_ICON_TRACE=1` logs the first 32 prompt calls (caller main+offset, asset, old -> new texture, reason kept) and
  the first 16 icon lookup calls. **The code module must be generated again** (both callers' chunks change); an older module logs
  "button prompts: not applied (... generate it again ...)". Cost: one walk of the asset's keys (two small hash lookups each) per
  prompt call, only with xbox / playstation; not measured. Tests: `python3 -m unittest tests.test_button_layout` (both generated callers
  through CALLDYN, PlayStation / Xbox, missing / NULL / ambiguous / malformed maps (capacities below counts and a freed but still
  hashed entry included, review regressions), no asset read after the call, registers and every
  other guest byte equal to the unwrapped run, trace opt-in and bounded; the install words are checked against `main.bin` when it is
  present, `ORBITALS_MAIN_BIN`).
- The game ships every icon: `DA_PlatformInputIcons` maps each FKey to a texture per `EPlatformType`
  (Desktop 0, Playstation 1, Xbox 2, Switch 3). The Switch 2 build always draws the Switch entry (which reuses the Xbox
  letter textures, swapped to the Nintendo positions).
- `button_layout=playstation|xbox` also wraps the separate per-key route (no memory scan):
  explicit per-key icon lookups go through `UMoreporkInputIconDataAsset` lookup main+0x40c5ce0
  `(asset, const FKey*, EPlatformType, UTexture2D** out) -> bool` (InputIcons TMap at asset+0x30, 0x70-byte elements; the
  per-platform TMap inside, 0x18-byte elements: key byte, texture at +8). Its only callers: the `GetIconBrushForKey` exec
  (main+0x40b2d20, bl at 0x40b2e64: Blueprint prompts) and the native icon widget (bl at 0x4634a78: asks Switch 3 through the shared
  `return 3` stub 0x4c4f0 when it has an owning player, Xbox 2 when not). `InputIcons` is CPF_Edit only (not Blueprint visible).
  The initial search missed the action-prompt copy because it compares against a constant rather than a register;
  that separate route is handled above. The runtime wraps the per-key function: a Switch request (and, for
  PlayStation, the widget's Xbox fallback) looks up 1 (PlayStation) / 2 (Xbox) first and falls back to the original platform when the key
  has no entry or its texture is NULL (the output is put back first). Nothing in guest memory is written. Fail closed: the entry, both
  element strides, the key / texture loads and the two callers' `bl` words are checked; a miss logs "button icons: not applied (...)"
  and the icons stay Nintendo.
- Build requirement: both callers reach the lookup with a direct `bl`, which the generated code compiles to a direct C call that never
  looks at `g_disp`. `codegen.py` `DYNAMIC_CALLS` lists main+0x40c5ce0 for this build id, so those two calls go through the dispatch
  table (`CALLDYN`) and the module exports `swr_dynamic_calls`. **The game code (`Orbitals_code.dll`) must be generated again with this
  SwitchRecomp** (`switchrecomp.py` on the game folder, as at install); only the two C chunks holding the callers change, so a kept
  build folder recompiles just those. A runtime with an older code module logs "button icons: not applied (the game code calls the icon
  lookup directly ...)" and keeps Nintendo icons. This lookup is kept for the paths that do use it, but the PC runs show the menu and
  co-op prompts do not (they take the action prompt above). Test: `python3 -m unittest tests.test_button_layout` (generated caller, wrapper,
  NULL / missing fallback, and the old direct call shown to bypass it).
- Log: "Orbitals: button icons: PlayStation icons (icon lookup main+40c5ce0 wrapped)"; once, if any key lacks the layout's icon,
  "a key without a PlayStation icon keeps the platform 3 one".
- Before (removed): a low-priority thread scanned all guest memory once a second for those 4-entry maps and rewrote their Switch
  texture. It cost 10.7% frames over 17.7 ms at 1800p co-op (see Performance below).
- With `button_accept_bottom=1`, the PC runs accept title/main/lobby and PlayStation Settings with the bottom button,
  return with the right button, and use the selected layout's glyphs. Xbox player two's pause/resume flow also passes.
  Xbox on-foot controls were subsequently exercised in run20261126-121131-332d70f3: holding the right button
  exits both ship seats; bottom-button presses make both characters jump (`xbox-both-jumping_s.png`).
  Pause and right-button return also work on foot (`xbox-onfoot-paused_s.png`, `xbox-onfoot-resumed_s.png`).
  This does not exhaust every gameplay interaction.

## Controller artwork limitation (observed on the PC)

The join screen still draws Nintendo Pro/Joy-Con controllers although the prompts beneath them correctly show
Cross/Circle or A/B. A precise image-widget hook was implemented and independently reviewed (source fbf2512,
integration8541377), then installed with a regenerated module and checked for both layouts.
The real wrapper at main+0x40e13e0 ran for types3/4, but the widget's map had no nonnull image for Xbox0 or
PlayStation2/1. Both runs logged `keeps the Nintendo drawing (no image for the layout)` and kept the original image.
Screenshots: `/tmp/claude-1000/orb/art-ps-controllers_s.png` and `art-xbox-join_s.png`.
Logs: `/tmp/claude-1000/bench/art-ps-final/Orbitals.log` and the isolated Xbox run20261126-121131-332d70f3.
The ineffective hook is reverted from the final runtime; its developer branch and commits remain local for continuation.
Completing this part needs a verified asset replacement/overlay supplying those drawings, not another platform-number change.
No replacement asset has been built or validated. GoalC is therefore partial despite working prompts and menu conventions.

## Accept on the bottom button (live menus verified; gameplay coverage partial)
- The Switch 2 build redirects six Enhanced Input mapping contexts to Switch versions (`BP_Switch2InputPlatformData`
  MappingContextRedirects: IMC_UI_Default, IMC_UI_CRU, IMC_Default, IMC_Cutscene, IMC_SpiritJug, IMC_SettlementGames ->
  `*_PlatformSwitch`) and lists the Switch versions as its UI contexts (`MoreporkDeveloperSettings` PlatformMappingContexts).
  In game the two sets differ only in cancel-type actions (accept/back in UI; InteractCancel, ExitSeat): jump is the bottom
  button on both, so a global A/B swap would move jump and is not an option.
- Tried and ignored by the game: Unreal's user layer `Saved/Config/Switch2/Input.ini` clearing `InputData`; the Generated
  platform layer `Platforms/Switch2/Config/GeneratedSwitch2Input.ini`; the pak's own `Switch2Input.ini` replaced (patched pak,
  `/tmp` tool, booted fine): the redirect data does not come from the config at run time. CommonInput platform settings in the
  same layers did not change the icons either.
- `button_accept_bottom=1`: the runtime points each redirect of `UEnhancedInputPlatformData` objects (vtable
  main+0x7fecb38, TMap at +0x28) at its own source context as they load (wrappers on the object's virtual functions, see below; the
  memory scan that also did it is removed) and the user
  Game.ini gives the UI the PC contexts. The EKeys initializer hook below also swaps CommonUI's accept/back keys.
  The latest PC run confirms bottom acceptance on title, main menu and lobby, and right-button back from lobby.
  Six redirects are rewritten before use. Selected glyphs now work; full gameplay coverage remains pending.
  The runtime option is off by default. The installed launcher ties bottom acceptance to Xbox/PlayStation.

## Next steps (not done)
- Player two's in-game Settings flow is verified above; the pre-join front-end ignores its navigation.
  The module has been regenerated, the action-prompt trace and real replacement glyphs observed.
  Validate on-foot jump and remaining gameplay interactions without advancing the fixed benchmark save.

## Review notes (Codex, 2026-11-26)
- (Historical) the memory scan ran under the runtime's memory lock per 1 MB chunk; it is removed (scanner-free icon lookup above).
- Remaining implementation limitation for `button_accept_bottom`: the virtual-function wrappers are not narrow: the class overrides
  only its two destructors (slots 0 / 1, never wrapped), so every other slot is a UObject function shared with thousands of classes
  and each of their calls pays the wrapper's check. The map write itself is one aligned store per live element (value := key, same
  class checked), counters, free list and hash untouched. The narrow fix is the C++ consumer of the redirects (Enhanced Input's
  rebuild) through `DYNAMIC_CALLS`; it is not found yet (the native `GetContextRedirect` 0x37fc7a0 is only reached from its exec).

## Platform accept / back keys (built and booted on the PC)
- `EKeys::Virtual_Accept` main+0x94ed438 and `Virtual_Back` main+0x94ed450 are 0x18-byte `FKey` copies (FName, KeyDetails pointer,
  reference controller) of `Gamepad_FaceButton_Right` main+0x94ec040 and `Gamepad_FaceButton_Bottom` main+0x94ec028, made at the end
  of the `EKeys` static initializer main+0x50dc00 (0x51188c-0x5118e0, returns 0x5119a4; its only reference is the init array entry
  at main+0x7ee3150). The binary has no "Virtual_Accept" name: on Switch accept = right is decided at compile time.
- Readers: directly (adrp/add) 0x388b07c / 0x388b0d0 (`FCommonAnalogCursor::HandleKeyDownEvent`: IsRelevantInput vtable +0x78,
  ActionRouter at +0x70 ProcessInput vtable +0x2e8, then `FAnalogCursor::HandleKeyDownEvent` 0x6df4f0), 0x388bca0 (key up),
  0x40c60bc; and through the GOT (main+0x8fa4680 = &Virtual_Accept, main+0x8fa4688 = &Virtual_Back), which the earlier adrp-only
  scan missed: 0x6e8bec / 0x6e8c2c (`FNavigationConfig` constructor: KeyActionRules Enter / Virtual_Accept -> Accept, Escape /
  Virtual_Back -> Back), 0x6de4d0 / 0x6df5a4 (`FAnalogCursor` click), 0x388ac2c (CommonUI, accept), 0x38befd8 (CommonUI, back).
- `button_accept_bottom=1` (with xbox / playstation) now wraps that initializer: after it returns it swaps the two globals (each
  reference controller stays referenced once, so counts and the exit destructors are unchanged), on the main thread before Slate,
  CommonUI or any other reader exists. Fail closed: the entry and the two copy sequences are checked by instruction word, the GOT must
  point at the two globals, Virtual_Accept must equal FaceButton_Right and Virtual_Back FaceButton_Bottom byte for byte; any miss logs
  "accept on the bottom button: not applied (...)" and writes nothing.
- Not touched: Enhanced Input mappings (jump stays on the bottom button), and the game's own `bInvertFaceBottomFaceRight`
  (settings object +0xe1, setter 0x4602420 remaps every FaceButton_Bottom / Right binding, jump included).
- Enhanced Input redirects: no longer emptied. Each live redirect element's target is set to its source (one aligned 8-byte store per
  element, both must be objects of the same class); count, free list and hash are unchanged. Live elements come from the allocation
  flags: the TBitArray's heap pointer (+0x48) whenever it is set, else its four inline words (+0x38), whatever the bit count.
- Lifetime: vtable slots 0 / 1 (main+0x1684900 complete, 0x16849b0 deleting destructor, which frees the object) are not wrapped;
  the wrapper rewrites the map BEFORE calling the original, while the caller still holds its receiver, and reads nothing after the
  original returns (a callee that retires or frees the object cannot make it touch a dead object); the original gets the registers
  unchanged. Tested on the real install path (`ei_wrap_vtable`) with a callee that retires the object (tests/test_button_layout.py).
  The installed build logs six rewritten contexts before use, and bottom acceptance is observed in the lobby and Settings.
  This demonstrates the required timing on the tested paths; it is not proof that every future context is covered.
  A narrower PostLoad/consumer hook remains an unimplemented alternative.

## Performance: removed icon scan and measured replacement
- With `button_layout=playstation` the once-a-second whole-memory scan under the memory lock gives repeatable frame spikes at 1800p
  (34-36 fps, p95 ~64 ms, p99 ~160 ms) against Nintendo 47 fps / p95 26 ms. It must not stay the final design.
- The scanner-free replacement is installed and observed. The per-key lookup alone did not cover menu/gameplay prompts;
  the action lookup at main+0x40c5020 required its own hook, described above. `GetPlatformIcons` / `SetPlatformIcons`
  (exec0x449db80/0x449e150, field+0xd8) is only an on/off setting, not the platform selector.
- [LAYOUT_BENCH.md](LAYOUT_BENCH.md) records eight alternating Nintendo/PlayStation runs with bottom acceptance enabled
  for PlayStation. There is no consistent FPS regression outside the repeated-run spread at1800p or2160p.
  The action wrapper performs bounded map validation per prompt; it is not free. Both layouts remain below stable60.
- `button_accept_bottom=1` still uses the virtual-function wrappers on `UEnhancedInputPlatformData` (now its only way to rewrite the
  redirects, since the scan that also caught them is gone; destructors excluded, see above). The precise consumer (`UEnhancedInputPlatformSettings` collecting the
  redirects into `RebuildControlMappings`) was searched for, not found yet: `GetContextRedirect` (exec 0x37fc710, native 0x37fc7a0)
  is only called from Blueprint; the settings object has `InputData` at +0x40 and `InputDataClasses` at +0x50 (reflection params at
  main+0x85579d8 / 0x8557a58), `bShouldLogMappingContextRedirects` param at 0x8557a90. PC check: the log must show
  "N Switch mapping context redirects of the platform data at ... now keep the PC contexts" once the lobby loads.
