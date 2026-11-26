# Button layout (Nintendo / Xbox / PlayStation) — what works and why

Ini keys (Orbitals.ini, launcher Controls page): `button_layout=nintendo|xbox|playstation`, `button_accept_bottom=0|1`.
Code: `runtime/orbitals_input.c` (button layout section). Measured on the gaming PC on 2026-11-26.

## Icons (works)
- The game ships every icon: `DA_PlatformInputIcons` maps each FKey to a texture per `EPlatformType`
  (Desktop 0, Playstation 1, Xbox 2, Switch 3). The Switch 2 build always draws the Switch entry (which reuses the Xbox
  letter textures, swapped to the Nintendo positions).
- `button_layout=playstation|xbox`: a low-priority runtime thread scans the game's memory once a second for those 4-entry
  maps (key byte 0..3, texture pointer, hash ids, four textures of one class) and copies the PlayStation / Xbox texture into
  the Switch entry. The game keeps building new copies for its prompts (about 50 a second in menus), hence the scan for the
  whole session (0.1-0.3 s a second on one low priority thread).
- Buttons keep their places: accept stays on the right button (Circle / Xbox B) and the prompts show exactly that
  ("○ Comenzar" on the title screen). Checked: title, main menu, lobby, prompts in co-op (screenshots in the night report).

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
- `button_accept_bottom=1`: the runtime empties the redirect maps of `UEnhancedInputPlatformData` objects (vtable
  main+0x7fecb38, TMap at +0x28) as they load (wrappers on the object's virtual functions + the memory scan) and the user
  Game.ini gives the UI the PC contexts. Result: lobby and later screens accept with the bottom button and go back with the
  right one, prompts matching. CommonUI's own buttons (title "Comenzar", main menu items, the lobby's "Comenzar") keep the
  platform's accept key (right button): that comes from the platform's `bSwapForwardAndBackButtons`, not Enhanced Input.
  So with this switch the first screens still need the right button: experimental, off by default.

## Next steps (not done)
- CommonUI accept: find where the engine reads `bSwapForwardAndBackButtons` / `GetGamepadAcceptKey` in the binary and patch
  the value at start (a data patch like the redirect one), then `button_accept_bottom` can become the PlayStation default.

## Review notes (Codex, 2026-11-26)
- The memory scan now runs under the runtime's memory lock per 1 MB chunk (no unmap during the read) and only patches maps
  whose four textures belong to the texture class learned at the first full match.
- Open for `button_accept_bottom` (why it stays experimental): the redirect maps are emptied by writing the TSparseArray
  counters from another thread (not transactional with the game's readers); the TSet hash buckets are left as they were, so a
  later Add on the same map could misbehave (the game is not seen adding to it after load); the virtual-function wrappers add a
  check to every call of those shared UObject functions.

## Platform accept / back keys (built 2026-11-26, offline reading of `main.bin`; not yet run on the PC)
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
  element, both must be objects of the same class); count, free list and hash are unchanged.

## Performance: the icon scan (parent's measurement, 2026-11-26)
- With `button_layout=playstation` the once-a-second whole-memory scan under the memory lock gives repeatable frame spikes at 1800p
  (34-36 fps, p95 ~64 ms, p99 ~160 ms) against Nintendo 47 fps / p95 26 ms. It must not stay the final design.
- Scanner-free route (not done): the icons are native: `UMoreporkInputIconDataAsset`, `UMoreporkInputIcon`,
  `UMoreporkInputIconRichTextDecorator`, `UMoreporkInputIconCombo` (names in the reflection data). Find the native lookup that does
  `TMap<EPlatformType, UTexture2D*>::Find(Switch=3)` (start from the UFunction / property tables of those classes, e.g. the
  `InputIconData` property at main+0x86df628) and hook it to look up 1 (PlayStation) or 2 (Xbox); fall back to 3 when that entry is
  empty. `GetPlatformIcons` / `SetPlatformIcons` (exec 0x449db80 / 0x449e150, field +0xd8) is only a bool user setting, not the type.
