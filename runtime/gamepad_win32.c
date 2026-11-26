/* Win32 gamepads. SDL handles HID and XInput as one inventory. XInput alone
   remains available on installations without the optional SDL3 DLL. */
#include "gamepad_win32.h"
#include <windows.h>
#include <xinput.h>
#include <stdbool.h>

typedef struct SDL_Gamepad SDL_Gamepad;
static struct {
  bool (*Init)(u32);
  u32* (*GetGamepads)(int*);
  u32* (*GetJoysticks)(int*);
  bool (*IsGamepad)(u32);
  const char* (*GetJoystickNameForID)(u32);
  u16 (*GetJoystickVendorForID)(u32);
  u16 (*GetJoystickProductForID)(u32);
  SDL_Gamepad* (*OpenGamepad)(u32);
  void (*CloseGamepad)(SDL_Gamepad*);
  bool (*GamepadConnected)(SDL_Gamepad*);
  bool (*GetGamepadButton)(SDL_Gamepad*,int);
  i16 (*GetGamepadAxis)(SDL_Gamepad*,int);
  bool (*RumbleGamepad)(SDL_Gamepad*,u16,u16,u32);
  void (*SetJoystickEventsEnabled)(bool);
  void (*SetGamepadEventsEnabled)(bool);
  void (*UpdateGamepads)(void);
  void (*free)(void*);
  /* optional (motion): */
  bool (*GamepadHasSensor)(SDL_Gamepad*,int);
  bool (*SetGamepadSensorEnabled)(SDL_Gamepad*,int,bool);
  bool (*GetGamepadSensorData)(SDL_Gamepad*,int,float*,int);
  void (*SetEventEnabled)(u32,bool);
  int (*PeepEvents)(void*,int,int,u32,u32);
} sdl;
/* SDL3 constants: SDL_SENSOR_ACCEL / GYRO, SDL_EVENT_GAMEPAD_SENSOR_UPDATE, SDL_GETEVENT */
enum { SDL_ACCEL=1, SDL_GYRO=2, SDL_EV_SENSOR=0x659, SDL_GET=2 };
/* SDL_GamepadSensorEvent inside the 128 byte SDL_Event */
typedef struct { u32 type, reserved; u64 timestamp; u32 which; i32 sensor; float data[3]; u32 pad_; u64 sensor_timestamp; u8 rest[128-48]; } SdlSensorEvent;
static int motion_on, motion_events;
enum { MOTION_RING=256 };
static struct { PadMotion s[MOTION_RING]; int head, n; float accel[3]; int sensors, events; u64 last_event_ns; } motion[WIN_GAMEPAD_SLOTS];
static bool sdl_active;
static struct { u32 id; SDL_Gamepad* handle; } sdl_slots[WIN_GAMEPAD_SLOTS];
static ULONGLONG next_scan;
static DWORD (WINAPI *xinput_get)(DWORD, XINPUT_STATE*);
static DWORD (WINAPI *xinput_set)(DWORD, XINPUT_VIBRATION*);
static u32 xinput_slots[WIN_GAMEPAD_SLOTS];

static bool load_sdl(void){
  HMODULE lib=LoadLibraryExW(L"SDL3.dll",NULL,LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if(!lib) return false;
#define SDL_BIND(name) do { \
    sdl.name=(__typeof__(sdl.name))GetProcAddress(lib,"SDL_" #name); \
    if(!sdl.name){ FreeLibrary(lib); return false; } \
  } while(0)
  SDL_BIND(Init);
  SDL_BIND(GetGamepads);
  SDL_BIND(GetJoysticks);
  SDL_BIND(IsGamepad);
  SDL_BIND(GetJoystickNameForID);
  SDL_BIND(GetJoystickVendorForID);
  SDL_BIND(GetJoystickProductForID);
  SDL_BIND(OpenGamepad);
  SDL_BIND(CloseGamepad);
  SDL_BIND(GamepadConnected);
  SDL_BIND(GetGamepadButton);
  SDL_BIND(GetGamepadAxis);
  SDL_BIND(SetJoystickEventsEnabled);
  SDL_BIND(SetGamepadEventsEnabled);
  SDL_BIND(UpdateGamepads);
  SDL_BIND(free);
#undef SDL_BIND
  sdl.RumbleGamepad=(__typeof__(sdl.RumbleGamepad))GetProcAddress(lib,"SDL_RumbleGamepad");
#define SDL_OPT(name) sdl.name=(__typeof__(sdl.name))GetProcAddress(lib,"SDL_" #name)
  SDL_OPT(GamepadHasSensor); SDL_OPT(SetGamepadSensorEnabled); SDL_OPT(GetGamepadSensorData); SDL_OPT(SetEventEnabled); SDL_OPT(PeepEvents);
#undef SDL_OPT
  if(!sdl.Init(0x2000)){ FreeLibrary(lib); return false; } /* SDL_INIT_GAMEPAD */
  sdl.SetGamepadEventsEnabled(false);
  sdl.SetJoystickEventsEnabled(false);
  return true;
}

void win_gamepads_init(void){
  const wchar_t* libraries[]={L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
  for(unsigned i=0;i<sizeof libraries/sizeof libraries[0] && !xinput_get;i++){
    HMODULE lib=LoadLibraryExW(libraries[i],NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(lib){
      xinput_get=(__typeof__(xinput_get))GetProcAddress(lib,"XInputGetState");
      if(xinput_get) xinput_set=(__typeof__(xinput_set))GetProcAddress(lib,"XInputSetState");
    }
  }
  const char* backend=getenv("SWR_GAMEPAD_BACKEND");
  if(!backend || strcmp(backend,"xinput")) sdl_active=load_sdl();
}

const char* win_gamepads_backend(void){
  return sdl_active ? "SDL3" : xinput_get ? "XInput" : "unavailable";
}

static i32 deadzone(i32 value){ return value>-7000 && value<7000 ? 0 : value; }
static i32 sdl_vertical(i16 value){ return deadzone(value==INT16_MIN ? INT16_MAX : -value); }

static void sdl_scan(void){
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    if(!sdl_slots[i].handle || sdl.GamepadConnected(sdl_slots[i].handle)) continue;
    sdl.CloseGamepad(sdl_slots[i].handle);
    sdl_slots[i].handle=NULL;
    sdl_slots[i].id=0;
    memset(&motion[i],0,sizeof motion[i]);
  }
  u32* ids=sdl.GetGamepads(NULL);
  if(!ids) return;
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    if(sdl_slots[i].id) continue;
    for(const u32* id=ids;*id;id++){
      if(*id==sdl_slots[1-i].id) continue;
      SDL_Gamepad* handle=sdl.OpenGamepad(*id);
      if(!handle) continue;
      sdl_slots[i].id=*id;
      sdl_slots[i].handle=handle;
      memset(&motion[i],0,sizeof motion[i]);
      break;
    }
  }
  sdl.free(ids);
}

static void sdl_pad(int i, WinGamepad* out){
  SDL_Gamepad* pad=sdl_slots[i].handle;
  if(!pad || !sdl.GamepadConnected(pad)) return;
  out->id=sdl_slots[i].id;
  const char* name=sdl.GetJoystickNameForID(out->id);
  snprintf(out->name,sizeof out->name,"%s",name ? name : "SDL gamepad");
  out->vendor=sdl.GetJoystickVendorForID(out->id);
  out->product=sdl.GetJoystickProductForID(out->id);
  /* Switch button bits indexed by SDL's positional button enum. */
  static const int bits[]={1,0,3,2,11,-1,10,4,5,6,7,13,15,12,14};
  for(unsigned b=0;b<sizeof bits/sizeof bits[0];b++)
    if(bits[b]>=0 && sdl.GetGamepadButton(pad,(int)b)) out->pad.buttons|=1ull<<bits[b];
  for(int b=0;b<21;b++) if(sdl.GetGamepadButton(pad,b)) out->sdl_buttons|=1u<<b;
  out->has_gyro=motion[i].sensors;
  out->pad.lx=deadzone(sdl.GetGamepadAxis(pad,0));
  out->pad.ly=sdl_vertical(sdl.GetGamepadAxis(pad,1));
  out->pad.rx=deadzone(sdl.GetGamepadAxis(pad,2));
  out->pad.ry=sdl_vertical(sdl.GetGamepadAxis(pad,3));
  out->zl=(u16)sdl.GetGamepadAxis(pad,4);
  out->zr=(u16)sdl.GetGamepadAxis(pad,5);
}

static void xinput_poll(WinGamepad pads[WIN_GAMEPAD_SLOTS]){
  if(!xinput_get) return;
  u32* slots=xinput_slots;
  XINPUT_STATE states[XUSER_MAX_COUNT];
  bool connected[XUSER_MAX_COUNT];
  for(DWORD i=0;i<XUSER_MAX_COUNT;i++) connected[i]=xinput_get(i,&states[i])==ERROR_SUCCESS;
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++)
    if(slots[i] && !connected[slots[i]-1]) slots[i]=0;
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    if(!slots[i]){
      for(u32 id=1;id<=XUSER_MAX_COUNT;id++)
        if(connected[id-1] && id!=slots[1-i]){ slots[i]=id; break; }
    }
    if(!slots[i]) continue;
    WinGamepad* out=&pads[i];
    const XINPUT_GAMEPAD* pad=&states[slots[i]-1].Gamepad;
    static const WORD buttons[]={
      XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_Y, XINPUT_GAMEPAD_X,
      XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_LEFT_SHOULDER,
      XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
      XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN
    };
    for(unsigned b=0;b<sizeof buttons/sizeof buttons[0];b++)
      if(pad->wButtons&buttons[b]) out->pad.buttons|=1ull<<b;
    out->id=slots[i];
    snprintf(out->name,sizeof out->name,"XInput %u",slots[i]-1);
    out->pad.lx=deadzone(pad->sThumbLX); out->pad.ly=deadzone(pad->sThumbLY);
    out->pad.rx=deadzone(pad->sThumbRX); out->pad.ry=deadzone(pad->sThumbRY);
    out->zl=(u16)(pad->bLeftTrigger*32767u/255u); out->zr=(u16)(pad->bRightTrigger*32767u/255u);
  }
}

static void motion_push(int i, u64 t, const float* gyro){
  PadMotion* m=&motion[i].s[(motion[i].head+motion[i].n)%MOTION_RING];
  m->t_ns=t; memcpy(m->gyro,gyro,sizeof m->gyro); memcpy(m->accel,motion[i].accel,sizeof m->accel);
  if(motion[i].n<MOTION_RING) motion[i].n++; else motion[i].head=(motion[i].head+1)%MOTION_RING;
}

/* every sensor sample arrives as an SDL event with its own timestamp; without events (an older SDL3) the latest reading,
   timed by this poll */
static void motion_poll(void){
  if(!sdl.GamepadHasSensor || !sdl.SetGamepadSensorEnabled || !sdl.GetGamepadSensorData) return;
  if(!motion_events && sdl.SetEventEnabled && sdl.PeepEvents){ sdl.SetEventEnabled(SDL_EV_SENSOR,true); motion_events=1; }
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    SDL_Gamepad* pad=sdl_slots[i].handle;
    if(!pad || motion[i].sensors) continue;
    if(sdl.GamepadHasSensor(pad,SDL_GYRO) && sdl.SetGamepadSensorEnabled(pad,SDL_GYRO,true)){
      if(sdl.GamepadHasSensor(pad,SDL_ACCEL)) sdl.SetGamepadSensorEnabled(pad,SDL_ACCEL,true);
      motion[i].sensors=1;
    } else motion[i].sensors=-1;   /* no gyro: not asked again until it reconnects */
  }
  u64 now=plat_ns();
  if(motion_events){
    SdlSensorEvent ev[64]; int n;
    while((n=sdl.PeepEvents(ev,64,SDL_GET,SDL_EV_SENSOR,SDL_EV_SENSOR))>0){
      for(int k=0;k<n;k++) for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
        if(!sdl_slots[i].id || ev[k].which!=sdl_slots[i].id) continue;
        motion[i].events=1; motion[i].last_event_ns=now;
        if(ev[k].sensor==SDL_ACCEL) memcpy(motion[i].accel,ev[k].data,sizeof motion[i].accel);
        else if(ev[k].sensor==SDL_GYRO) motion_push(i,ev[k].sensor_timestamp? ev[k].sensor_timestamp : ev[k].timestamp,ev[k].data);
      }
      if(n<64) break;
    }
  }
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    SDL_Gamepad* pad=sdl_slots[i].handle;
    if(!pad || motion[i].sensors!=1 || motion[i].events) continue;
    /* Once events have worked, silence means stale data; do not resurrect a cached rate with a fresh host timestamp. */
    float g[3];
    sdl.GetGamepadSensorData(pad,SDL_ACCEL,motion[i].accel,3);
    if(sdl.GetGamepadSensorData(pad,SDL_GYRO,g,3)) motion_push(i,now,g);
  }
}

void win_gamepads_motion_enable(int on){
  if(!sdl_active || motion_on==!!on) return;
  motion_on=!!on;
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    SDL_Gamepad* pad=sdl_slots[i].handle;
    if(!on && pad && motion[i].sensors==1 && sdl.SetGamepadSensorEnabled){ sdl.SetGamepadSensorEnabled(pad,SDL_GYRO,false); sdl.SetGamepadSensorEnabled(pad,SDL_ACCEL,false); }
    memset(&motion[i],0,sizeof motion[i]);
  }
  if(!on && motion_events && sdl.SetEventEnabled){ sdl.SetEventEnabled(SDL_EV_SENSOR,false); motion_events=0; }
}

int win_gamepads_motion(int slot, PadMotion* out, int max){
  if(slot<0 || slot>=WIN_GAMEPAD_SLOTS) return 0;
  int n=motion[slot].n<max? motion[slot].n : max;
  for(int k=0;k<n;k++) out[k]=motion[slot].s[(motion[slot].head+k)%MOTION_RING];
  motion[slot].head=(motion[slot].head+n)%MOTION_RING; motion[slot].n-=n;
  return n;
}

void win_gamepads_poll(WinGamepad pads[WIN_GAMEPAD_SLOTS]){
  memset(pads,0,sizeof *pads*WIN_GAMEPAD_SLOTS);
  if(sdl_active){
    sdl.UpdateGamepads();
    ULONGLONG now=GetTickCount64();
    if(now>=next_scan){ sdl_scan(); next_scan=now+250; }
    if(motion_on) motion_poll();
    for(int i=0;i<WIN_GAMEPAD_SLOTS;i++) sdl_pad(i,&pads[i]);
  }else xinput_poll(pads);
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    if(pads[i].zl>7710) pads[i].pad.buttons|=1ull<<8;
    if(pads[i].zr>7710) pads[i].pad.buttons|=1ull<<9;
  }
}

int win_gamepads_rumble(int player, u16 low, u16 high, u32 duration_ms){
  if(sdl_active){
    SDL_Gamepad* pad=sdl_slots[player].handle;
    return pad && sdl.RumbleGamepad && sdl.RumbleGamepad(pad,low,high,duration_ms);
  }
  u32 id=xinput_slots[player];
  XINPUT_VIBRATION motors={low,high};
  return id && xinput_set && xinput_set(id-1,&motors)==ERROR_SUCCESS;
}

void win_gamepads_inventory(void){
  printf("backend\t%s\n",win_gamepads_backend());
  if(sdl_active){
    u32* ids=sdl.GetJoysticks(NULL);
    if(ids){
      for(const u32* id=ids;*id;id++){
        gamepad_report(sdl.GetJoystickNameForID(*id),sdl.IsGamepad(*id));
      }
      sdl.free(ids);
    }
    return;
  }
  if(xinput_get) for(DWORD i=0;i<XUSER_MAX_COUNT;i++){
    XINPUT_STATE state;
    if(xinput_get(i,&state)==ERROR_SUCCESS){
      char name[32];
      snprintf(name,sizeof name,"XInput controller %lu",(unsigned long)i+1);
      gamepad_report(name,1);
    }
  }
}
