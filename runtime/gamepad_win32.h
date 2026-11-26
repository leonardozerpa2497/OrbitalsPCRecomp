#pragma once
#include "rt.h"
#include "orbitals_input.h"

enum { WIN_GAMEPAD_SLOTS = 2 };

typedef struct {
  u32 id;
  u16 vendor, product;
  char name[128];
  PadState pad;
  u16 zl, zr;
  u32 sdl_buttons;   /* SDL_GamepadButton bits (SDL only), for buttons the Switch layout lacks (touchpad, paddles) */
  int has_gyro;      /* motion samples available (win_gamepads_motion) */
} WinGamepad;

void win_gamepads_init(void);
const char* win_gamepads_backend(void);
void win_gamepads_poll(WinGamepad pads[WIN_GAMEPAD_SLOTS]);
int win_gamepads_rumble(int player, u16 low, u16 high, u32 duration_ms);
void win_gamepads_inventory(void);
/* motion sensors (SDL): turned on while wanted; the samples a pad slot sent since the last call, oldest first */
void win_gamepads_motion_enable(int on);
int win_gamepads_motion(int slot, PadMotion* out, int max);
