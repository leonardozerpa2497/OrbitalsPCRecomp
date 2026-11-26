# Button layout (Nintendo / Xbox / PlayStation) — what works and why

Ini keys (Orbitals.ini, launcher Controls page): `button_layout=nintendo|xbox|playstation`, `button_accept_bottom=0|1`.
Code: `runtime/orbitals_input.c` (button layout section). Measured on the gaming PC on 2026-11-26.

## Icons (not working in the current PC validation)
The 08:30-08:44 PC runs with PlayStation selected still show Nintendo B/A in title, main menu, lobby and settings, and ZR/Y in co-op.
The regenerated module exports the dynamic lookup and both generated caller chunks use `CALLDYN`, but the opt-in
`SWR_ICON_TRACE=1` logs no calls to the wrapper. The installation log alone is not proof that a prompt reaches it.
- Root cause (from `main.bin`, 2026-11-26): those prompts never call the icon lookup main+0x40c5ce0. They go through the action
  prompt main+0x40c5020 `(UInputAction*, asset, UTexture2D** pad, UTexture2D** keyboard, bool* label, FKey* labelKey, player) -> bool`,
  which gets the action's mapped keys (0x40cb140) and looks each one up in `InputIcons` itself, inlined, with the platform hard-coded:
  a gamepad key asks Switch 3 (`and w10, w10, #3` at 0x40c51ec, `cmp w8, #3` at 0x40c520c) and writes `*pad`; a keyboard key asks
  Desktop 0 and writes `*keyboard`, or, without one, the asset's default texture (+0x80), `*label = true` and a copy of that keyboard
  key into `*labelKey` (so x5 is never the gamepad key). Its only references are two direct `bl` (0x40c4ed0 in the input-action
  widget, 0x40d3990), both zeroing `*pad` first.
- Fix (built offline, not yet run on the PC): `codegen.py` `DYNAMIC_CALLS` now lists 0x40c5020 too, and the runtime wraps it
  (`icon_prompt`, runtime/orbitals_buttons.h; install with its own fail-closed word checks, `icon_prompt_install`). Before the call,
  while the caller holds the asset (exactly the original's own lifetime for it), it lists for every live `InputIcons` key (allocation
  bits) its Switch texture and that key's PlayStation / Xbox texture, through each key's platform-map hash as the game does. After the
  call it reads only `*pad`: if the call changed it to a Switch texture of that list, it writes the same key's layout texture. Kept
  Switch (fail closed): a Switch texture shared by keys whose replacements differ, a missing or NULL replacement, an unchanged slot, or
  a map that fails any bound (counts, free count, live count vs flags, inline flags past 128 bits, alignment / readability, hash size,
  index range, chain longer than the map). Registers, return value, `*keyboard`, `*label`, `*labelKey` and its reference count are the
  original's. Logs: "button prompts: PlayStation icons (action prompt main+40c5020 wrapped)" at start; once, "a prompt keeps its Switch
  icon (reason)". `SWR_ICON_TRACE=1` logs the first 32 prompt calls (caller main+offset, asset, old -> new texture, reason kept) and
  the first 16 icon lookup calls. **The code module must be generated again** (both callers' chunks change); an older module logs
  "button prompts: not applied (... generate it again ...)". Cost: one walk of the asset's keys (two small hash lookups each) per
  prompt call, only with xbox / playstation; not measured. Tests: `python3 -m unittest tests.test_button_layout` (both generated callers
  through CALLDYN, PlayStation / Xbox, missing / NULL / ambiguous / malformed maps, no asset read after the call, registers and every
  other guest byte equal to the unwrapped run, trace opt-in and bounded; the install words are checked against `main.bin` when it is
  present, `ORBITALS_MAIN_BIN`).
- The game ships every icon: `DA_PlatformInputIcons` maps each FKey to a texture per `EPlatformType`
  (Desktop 0, Playstation 1, Xbox 2, Switch 3). The Switch 2 build always draws the Switch entry (which reuses the Xbox
  letter textures, swapped to the Nintendo positions).
- `button_layout=playstation|xbox` (current design, no memory scan; built 2026-11-26 from `main.bin`, not yet run on the PC):
  every icon lookup goes through one native function, `UMoreporkInputIconDataAsset` lookup main+0x40c5ce0
  `(asset, const FKey*, EPlatformType, UTexture2D** out) -> bool` (InputIcons TMap at asset+0x30, 0x70-byte elements; the
  per-platform TMap inside, 0x18-byte elements: key byte, texture at +8). Its only callers: the `GetIconBrushForKey` exec
  (main+0x40b2d20, bl at 0x40b2e64: Blueprint prompts) and the native icon widget (bl at 0x4634a78: asks Switch 3 through the shared
  `return 3` stub 0x4c4f0 when it has an owning player, Xbox 2 when not). `InputIcons` is CPF_Edit only (not Blueprint visible), and a
  search of the text for the inlined two-level lookup (0x70 / 0x18 strides with a byte key) finds no other copy (the two other byte-keyed
  0x18 maps, 0x453efec / 0x4540f4c, are gameplay maps on actors). The runtime wraps that function: a Switch request (and, for
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
- With `button_accept_bottom=1`, the latest PC run accepts title/main/lobby with the bottom button and returns from the lobby
  with the right button. The glyphs remain Nintendo. Both-player gameplay and Xbox validation are still pending.

## Accept on the bottom button (experimental, partial)
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
  Six redirects are rewritten before use. Glyphs and full gameplay coverage remain pending; the option is off by default.

## Next steps (not done)
- Regenerate the code module, then on the PC: `SWR_ICON_TRACE=1` must show "icon prompt reached: caller main+40c4ed4 / 40d3994"
  with Switch -> PlayStation textures; screenshots of title / main / lobby / settings / co-op with PlayStation and Xbox; A/B bench
  Nintendo vs PlayStation for the per-prompt walk; validate both players.

## Review notes (Codex, 2026-11-26)
- (Historical) the memory scan ran under the runtime's memory lock per 1 MB chunk; it is removed (scanner-free icon lookup above).
- Open for `button_accept_bottom` (why it stays experimental): the virtual-function wrappers are not narrow: the class overrides
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
  Not shown yet: that the pre-call rewrite catches the maps as early as the post-call one did (the first virtual call after the
  properties are read must come before Enhanced Input reads the map). PC check: the "N Switch mapping context redirects ... now keep
  the PC contexts" log line once the lobby loads, and the bottom button accepting in the lobby. If it does not show, the fallback is
  a verified PostLoad-only hook (not built).

## Performance: the icon scan (parent's measurement, 2026-11-26)
- With `button_layout=playstation` the once-a-second whole-memory scan under the memory lock gives repeatable frame spikes at 1800p
  (34-36 fps, p95 ~64 ms, p99 ~160 ms) against Nintendo 47 fps / p95 26 ms. It must not stay the final design.
- Scanner-free route (not done): the icons are native: `UMoreporkInputIconDataAsset`, `UMoreporkInputIcon`,
  `UMoreporkInputIconRichTextDecorator`, `UMoreporkInputIconCombo` (names in the reflection data). Find the native lookup that does
  `TMap<EPlatformType, UTexture2D*>::Find(Switch=3)` (start from the UFunction / property tables of those classes, e.g. the
  `InputIconData` property at main+0x86df628) and hook it to look up 1 (PlayStation) or 2 (Xbox); fall back to 3 when that entry is
  empty. `GetPlatformIcons` / `SetPlatformIcons` (exec 0x449db80 / 0x449e150, field +0xd8) is only a bool user setting, not the type.
- Done (2026-11-26, offline): the scan is removed; icons go through the icon lookup wrapper above, at no cost outside icon lookups
  (one extra lookup only when a key lacks the layout's icon). Bench needed: same 1800p co-op scene, Nintendo vs PlayStation,
  alternated >= 2 runs each; expected PlayStation = Nintendo within spread (before: PlayStation 34.37 fps, p95 64.55, p99 163.46,
  spikes 10.70%, max 674 ms; Nintendo 48.30 fps, p95 25.37, p99 29.53, spikes 0.83%).
- `button_accept_bottom=1` still uses the virtual-function wrappers on `UEnhancedInputPlatformData` (now its only way to rewrite the
  redirects, since the scan that also caught them is gone; destructors excluded, see above). The precise consumer (`UEnhancedInputPlatformSettings` collecting the
  redirects into `RebuildControlMappings`) was searched for, not found yet: `GetContextRedirect` (exec 0x37fc710, native 0x37fc7a0)
  is only called from Blueprint; the settings object has `InputData` at +0x40 and `InputDataClasses` at +0x50 (reflection params at
  main+0x85579d8 / 0x8557a58), `bShouldLogMappingContextRedirects` param at 0x8557a90. PC check: the log must show
  "N Switch mapping context redirects of the platform data at ... now keep the PC contexts" once the lobby loads.
