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
