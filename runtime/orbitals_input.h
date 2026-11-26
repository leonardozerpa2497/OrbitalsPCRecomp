/* Orbitals PC Recomp, based on SwitchRecomp by MZ Labs. AGPL-3.0, see LICENSE.txt and NOTICE.txt. */
/* Orbitals input extras: button layout and gyro aiming (orbitals_input.c) */
#pragma once
#include "rt.h"

/* one motion sample of a pad: SDL sensor units (rad/s, m/s^2), +X right, +Y up, +Z toward the player */
typedef struct { u64 t_ns; float gyro[3], accel[3]; } PadMotion;

void orbitals_input_ini(const char* key, const char* value);
const char* orbitals_layout_overlay(void);        /* RomFS overlay folder for button_layout, or NULL */
void orbitals_input_install(const SwrModInfo* mod); /* binds the gyro to the game's camera (validated main build only) */
int orbitals_gyro_wanted(void);                     /* the pads' motion sensors are needed */
/* once a frontend frame, for each pad slot (player 1 = 0): its id (0 = none), VID:PID, raw SDL buttons
   (bit = SDL_GamepadButton), left trigger 0..32767, and the motion samples read since the last call */
void orbitals_gyro_pad(int slot, u32 id, u16 vendor, u16 product, u32 sdl_buttons, u16 zl, const PadMotion* m, int n, int has_gyro,
                      int allowed);   /* allowed: the game has the focus and no PC panel is open (otherwise nothing turns) */
