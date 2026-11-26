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

## WIP: where the accept key lives (offline binary reading, 2026-11-26, not built or run)
Read from `main.bin` (module offsets, same build as `orbitals_build`); nothing below is patched yet.
- `EKeys` static `FKey`s are 0x18 bytes (FName +0, KeyDetails object +8, reference controller +0x10), built by the
  `EKeys` static initializer (runs from 0x50f4xx, returns at 0x5119a4): `Gamepad_FaceButton_Bottom` = main+0x94ec028
  (0x50f560), `Gamepad_FaceButton_Right` = main+0x94ec040 (0x50f58c). x19 / x20 keep those two addresses until the end.
- At the end of that initializer (0x51188c-0x5118e0) two more `FKey`s are copied with a reference added:
  main+0x94ed438 <- `[x20]` (FaceButton_Right) and main+0x94ed450 <- `[x19]` (FaceButton_Bottom). The binary has no
  "Virtual_Accept" string in any encoding, which fits Unreal's `EKeys::Virtual_Accept = FPlatformInput::GetGamepadAcceptKey()` /
  `Virtual_Back = GetGamepadBackKey()`: on Switch accept = right, back = bottom (DataDrivenPlatformInfo
  `[InputInfo Switch2] bSwapForwardAndBackButtons=true` is a build-time key; its name is not in the binary).
- Readers (adrp/add/ldr xrefs): main+0x94ed438 (Virtual_Accept) is compared with a key event's FName (event +0x28) at
  0x388b07c / 0x388b0d0, 0x388bca0 and 0x40c60bc (not yet identified: likely CommonUI analog cursor / action router or a
  button's key handler). main+0x94ed450 (Virtual_Back) has NO runtime reader, only its init and its atexit destructor
  (0x50d8d8). So Slate / CommonUI "back" does not go through Virtual_Back, and "accept" is not only these 4 compares:
  the main accept / back decision (FNavigationConfig or the Switch platform's own) must test the physical keys directly.
- Other runtime readers of FaceButton_Right: 0x42bdc24 / 0x42bdd24 (key -> analog state table, not the decision).
  FaceButton_Bottom: 0x2cf7e44 (builds a key copy). The static-init sites 0x51c4bc/0x51c5ac (EKeys::Initialize AddKey) and
  0x52a378/0x52a3cc/0x52a8a0/0x52a8f4/0x52ad70/0x52adc4 (per-platform key display names) are not the decision either.
- Not a fix: `UOrbGameSettingGamepad` `bInvertFaceBottomFaceRight` (menu "Invert Face Bottom / Face Right") is the game's own
  remap and would move jump too (not checked in code).
- Continue: (1) identify the functions at 0x388b0xx, 0x388bcxx, 0x40c60xx (callers, vtables, nearby strings); (2) find the
  Slate navigation decision: xrefs of EKeys::SpaceBar (main+0x94eb458: readers 0x51346c, 0x2cf8074, 0x4210e60) — the
  function that also compares FaceButton_Right/Bottom or Virtual_Accept is FNavigationConfig::GetNavigationActionFromKey or
  its Switch override; (3) a candidate fix is to swap the two 0x18-byte FKeys at main+0x94ed438 / +0x94ed450 right after the
  EKeys initializer returns (refcount neutral), plus whatever the decision in (2) needs; the existing g_accept_bottom
  redirect emptying stays until that is measured. Tools: a numpy adrp/add/ldr xref scanner (scratch, not in git).
