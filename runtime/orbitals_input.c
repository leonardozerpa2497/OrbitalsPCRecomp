/* Orbitals PC Recomp, based on SwitchRecomp by MZ Labs. AGPL-3.0, see LICENSE.txt and NOTICE.txt. */
/* Orbitals input extras: the button layout (Nintendo / Xbox / PlayStation) and gyro aiming.
   Everything here is off by default (button_layout=nintendo, gyro=off): the game then runs as before. */
#include "rt.h"
#include "orbitals_input.h"
#include <math.h>

void ini_set(const char* k, const char* v);   /* main.c: stores a setting and rewrites the ini */
int code_dynamic_call(u64 pc);                /* main.c: the code module calls pc through g_disp */

/* ===================================================================== button layout
   Measured on the PC (2026-11-26, docs/dev4k/BUTTON_LAYOUT.md):
   - Icons: DA_PlatformInputIcons maps each key to a texture per EPlatformType (Desktop 0, Playstation 1, Xbox 2, Switch 3) and the Switch 2
     build always asks for the Switch one. button_layout=xbox / playstation wraps the game's icon lookup (icon_lookup) and the action
     prompt that inlines its own Switch lookup (icon_prompt) so that a Switch icon becomes that controller's texture for the same key
     (not shown on the PC yet, docs/dev4k/BUTTON_LAYOUT.md). Buttons keep their
     places unless button_accept_bottom is on, and the icons follow the key either way.
   - Accept on the bottom button (button_accept_bottom=1, experimental): the Switch 2 build redirects six Enhanced Input mapping contexts
     to Switch versions (BP_Switch2InputPlatformData, MappingContextRedirects) and its UI lists those Switch versions
     (MoreporkDeveloperSettings PlatformMappingContexts). The redirect data does not come from the config at run time (a user Input.ini,
     the Generated platform layer and a replaced Switch2Input.ini inside the pak were all ignored), so as the redirect maps load the
     runtime points each redirect at its own source context (ei_virtual) and gives the UI the PC contexts (user Game.ini):
     the lobby, the pause menu and the game then accept on the bottom button. CommonUI's own buttons (title screen, main menu,
     "Comenzar") and Slate navigation use the platform's accept / back keys: those two EKeys globals are swapped at start (ekeys_init).
   nintendo (default) changes nothing. */
static int g_layout;   /* 0 nintendo, 1 xbox, 2 playstation */
static int g_accept_bottom;   /* button_accept_bottom=1: Enhanced Input's Switch redirects taken out (experimental) */

static int write_text(const char* path, const char* text){
  FILE* f=fopen(path,"wb"); if(!f) return 0;
  fputs(text,f); fclose(f); return 1;
}

const char* orbitals_layout_overlay(void){
  if(!g_layout || !g_accept_bottom) return NULL;
  static char root[1200];
  const char* name= g_layout==1? "xbox" : "playstation";
  char dir[1400];
  snprintf(root,sizeof root,"%sbutton_layout",g_exedir); plat_mkdir(root);
  snprintf(root,sizeof root,"%sbutton_layout/%s",g_exedir,name); plat_mkdir(root);
  static const char* parts[]={"Orbitals","Saved","Config","Switch2"};
  size_t n=(size_t)snprintf(dir,sizeof dir,"%s",root);
  for(unsigned i=0;i<4 && n<sizeof dir;i++){ n+=(size_t)snprintf(dir+n,sizeof dir-n,"/%s",parts[i]); plat_mkdir(dir); }
  char path[1500];
  snprintf(path,sizeof path,"%s/Game.ini",dir);
  if(!write_text(path,
    "; written by the runtime (button_accept_bottom): the UI's input contexts are the PC version's ones\n"
    "[/Script/MoreporkUI.MoreporkDeveloperSettings]\n"
    "!PlatformMappingContexts=ClearArray\n"
    "+PlatformMappingContexts=/Game/Orbitals/Input/UI/IMC_UI_Default.IMC_UI_Default\n"
    "+PlatformMappingContexts=/Game/Orbitals/Input/IMC_Cutscene.IMC_Cutscene\n"
    "+PlatformMappingContexts=/Game/Orbitals/Input/IMC_SpiritJug.IMC_SpiritJug\n"
    "+PlatformMappingContexts=/Game/Orbitals/Input/IMC_SettlementGames.IMC_SettlementGames\n"
    "+PlatformMappingContexts=/Game/Orbitals/Input/UI/IMC_UI_CRU.IMC_UI_CRU\n"
    "+PlatformMappingContexts=/Game/Orbitals/Input/IMC_Default.IMC_Default\n")){
    LOG("Orbitals: button layout %s: could not write %s\n",name,dir); return NULL; }
  LOG("Orbitals: accept on the bottom button (Unreal user config from %s)\n",root);
  return root;
}

/* The Switch 2 mapping context redirects are not read from the config at run time (neither the user layer nor a replaced
   Switch2Input.ini changes them: measured 2026-11-26), so they are taken out of the loaded data instead. UEnhancedInputPlatformData
   (0x78 bytes, vtable main+0x7fecb38, built by main+0x16847a0) keeps MappingContextRedirects as a TMap at +0x28: TSparseArray data
   pointer +0x28, count +0x30, capacity +0x34, allocation bits (heap pointer +0x48 when set, else inline +0x38; bit count +0x50), first free +0x58,
   free count +0x5c. Each redirect's target is set to its source (IMC_UI_Default -> IMC_UI_Default): no element is added or removed, so
   the map's structure stays valid for its readers, and Enhanced Input keeps the PC version's contexts at its next rebuild. The game's own
   UI list (MoreporkDeveloperSettings PlatformMappingContexts, user Game.ini) is set to the same PC contexts so the prompts match. */
enum { EI_PLATFORM_DATA_VTABLE=0x7fecb38 };
static u64 g_ei_vtable;
int mem_with_range(u64 addr, u64 len, void (*fn)(u64 addr, u64 len, void* ctx), void* ctx);
int mem_span_ok_locked(u64 addr, u64 size, u32 perm);   /* inside mem_with_range only */
#include "orbitals_buttons.h"   /* ei_clear, ei_virtual / ei_wrap_vtable, icon_lookup, icon_prompt */
/* Button icons: DA_PlatformInputIcons (UMoreporkInputIconDataAsset) keeps InputIcons, a TMap<FKey, entry> (0x70-byte elements at
   +0x30) whose entry holds a TMap<EPlatformType, UTexture2D*> (Desktop 0, Playstation 1, Xbox 2, Switch 3; 0x18-byte elements: key
   byte, texture at +8). The per-key lookup is main+0x40c5ce0 (asset, const FKey*, EPlatformType, UTexture2D** out) -> bool, called
   only by the GetIconBrushForKey exec (Blueprint prompts, main+0x40b2e64) and the native icon widget (main+0x4634a78, which asks for
   Switch 3 when it has an owning player and Xbox 2 when not). It is NOT the only reader: the action prompt main+0x40c5020 inlines the
   same lookup (icon_prompt_install below), and the PC runs showed the prompts never reaching 0x40c5ce0. button_layout=xbox /
   playstation wraps that function: a Switch request (and, for playstation, the Xbox fallback) looks up the layout's entry first and
   falls back to the original request when the key has none or a NULL texture (icon_lookup, orbitals_buttons.h). No guest memory is
   written. Both callers reach it with a direct bl, which the generated code turns into a direct C call unless codegen.py lists the
   target in DYNAMIC_CALLS: then the call goes through g_disp, where the wrapper sits. The code module says which targets it built
   that way (swr_dynamic_calls); without it the wrapper would never run, so nothing is installed. */
enum { ICON_LOOKUP=0x40c5ce0, ICON_CALL_BP=0x40b2e64, ICON_CALL_WIDGET=0x4634a78 };
static void icon_install(const SwrModInfo* mod){
  /* entry pacibsp; stp x29, x30, [sp, #-0x40]!; mov w12, #0x70 (outer element); mov w10, #0x18 (inner element); ldrb w12, [x11]
     (platform key); ldr x8, [x11, #8] (texture); and the two callers' bl */
  static const struct { u32 off, w; } sig[]={ {0x40c5ce0,0xd503237f}, {0x40c5ce4,0xa9bc7bfd}, {0x40c5d78,0x52800e0c}, {0x40c5de4,0x5280030a},
                                               {0x40c5dec,0x3940016c}, {0x40c5e10,0xf9400568}, {ICON_CALL_BP,0x94004b9f}, {ICON_CALL_WIDGET,0x97ea449a} };
  for(unsigned i=0;i<sizeof sig/sizeof sig[0];i++){
    u64 a=mod->base+sig[i].off;
    if(a<g_code_lo || a>=g_code_hi || gr32(a)!=sig[i].w){ LOG("Orbitals: button icons: not applied (icon lookup not found, %x)\n",sig[i].off); return; }
  }
  u64 pc=mod->base+ICON_LOOKUP;
  GuestFn* e=&g_disp[(pc-g_code_lo)/4];
  if(!*e){ LOG("Orbitals: button icons: not applied (icon lookup has no entry)\n"); return; }
  if(!code_dynamic_call(pc)){ LOG("Orbitals: button icons: not applied (the game code calls the icon lookup directly: generate it again with this SwitchRecomp)\n"); return; }
  icon_lookup_orig=*e; *e=icon_lookup;
  LOG("Orbitals: button icons: %s icons (icon lookup main+%x wrapped)\n",g_layout==1?"Xbox":"PlayStation",ICON_LOOKUP);
}
/* The action prompt (icon_prompt, orbitals_buttons.h): main+0x40c5020 looks the action's keys up in the same InputIcons map with
   Switch 3 inlined (it never calls the icon lookup above), and it is what the title, menus, lobby, settings and co-op prompts reach
   (callers main+0x40c4ed0 and main+0x40d3990, both a direct bl, the only references in main.bin). Installed on its own, with its own
   checks: the entry, the arguments it keeps (x2 into x20, then [sp+8]), both strides, the outer map fields, the inner map fields and
   hash mask, the platform compare (cmp w8, #3), the texture load and its store to the pad slot, and the two callers' bl words. */
enum { ICON_PROMPT=0x40c5020, ICON_PROMPT_CALL_A=0x40c4ed0, ICON_PROMPT_CALL_B=0x40d3990 };
static void icon_prompt_install(const SwrModInfo* mod){
  static const struct { u32 off, w; } sig[]={
    {0x40c5020,0xd503237f}, {0x40c5024,0xd10243ff},                          /* pacibsp; sub sp, sp, #0x90 */
    {0x40c5060,0xaa0203f4}, {0x40c5088,0xf90007f4},                          /* mov x20, x2; str x20, [sp, #8] */
    {0x40c5084,0x52800e1a}, {0x40c50e4,0xb9403b08}, {0x40c50e8,0xb9406709},  /* mov w26, #0x70; ldr w8, [x24, #0x38]; ldr w9, [x24, #0x64] */
    {0x40c5120,0xf9401b09}, {0x40c5134,0xb9406908},                          /* ldr x9, [x24, #0x30]; ldr w8, [x8, #0x68] */
    {0x40c51c4,0xb9402109}, {0x40c51c8,0xb9404d0a}, {0x40c51d4,0xf9402d0a},  /* inner count +0x20, free +0x4c, hash heap +0x58 */
    {0x40c51d8,0x91014109}, {0x40c51e4,0xb940610a}, {0x40c51ec,0x1200054a},  /* hash inline +0x50, hash size +0x60, and w10, w10, #3 */
    {0x40c51fc,0xf9400d09}, {0x40c5200,0x52800308}, {0x40c5208,0x39400148},  /* inner data +0x18; mov w8, #0x18; ldrb w8, [x10] */
    {0x40c520c,0x71000d1f}, {0x40c5214,0xb940114a},                          /* cmp w8, #3; ldr w10, [x10, #0x10] */
    {0x40c5290,0xf9400548}, {0x40c5294,0xf94007e9}, {0x40c5298,0xf9000128},  /* ldr x8, [x10, #8]; ldr x9, [sp, #8]; str x8, [x9] */
    {ICON_PROMPT_CALL_A,0x94000054}, {ICON_PROMPT_CALL_B,0x97ffc5a4} };
  for(unsigned i=0;i<sizeof sig/sizeof sig[0];i++){
    u64 a=mod->base+sig[i].off;
    if(a<g_code_lo || a>=g_code_hi || gr32(a)!=sig[i].w){ LOG("Orbitals: button prompts: not applied (action prompt not found, %x)\n",sig[i].off); return; }
  }
  u64 pc=mod->base+ICON_PROMPT;
  GuestFn* e=&g_disp[(pc-g_code_lo)/4];
  if(!*e){ LOG("Orbitals: button prompts: not applied (action prompt has no entry)\n"); return; }
  if(!code_dynamic_call(pc)){ LOG("Orbitals: button prompts: not applied (the game code calls the action prompt directly: generate it again with this SwitchRecomp)\n"); return; }
  icon_prompt_orig=*e; *e=icon_prompt;
  LOG("Orbitals: button prompts: %s icons (action prompt main+%x wrapped)\n",g_layout==1?"Xbox":"PlayStation",ICON_PROMPT);
}
/* The platform's accept / back keys. Unreal builds EKeys::Virtual_Accept = FPlatformInput::GetGamepadAcceptKey() and Virtual_Back =
   GetGamepadBackKey() as copies of two face button FKeys at the end of the EKeys static initializer (main+0x50dc00, called once from
   the init array): on Switch accept = FaceButton_Right, back = FaceButton_Bottom (main+0x51188c-0x5118e0). Every runtime reader goes
   through those two globals (directly or by the GOT at main+0x8fa4680 / 0x8fa4688): FNavigationConfig's KeyActionRules (built later,
   when Slate starts), FAnalogCursor / FCommonAnalogCursor's click, CommonUI's action router accept / back. Enhanced Input mappings and
   the game's own bInvertFaceBottomFaceRight name the physical keys and are not touched, so jump stays on the bottom button.
   button_accept_bottom swaps the two 0x18-byte FKeys (FName, KeyDetails pointer, reference controller) right after the initializer
   returns, on the game's main thread before any other code reads them: each controller stays referenced once, by the other global,
   so the reference counts and the exit-time destructors are unchanged. Every check below must hold or nothing is written. */
enum { EKEYS_INIT=0x50dc00, EKEY_BOTTOM=0x94ec028, EKEY_RIGHT=0x94ec040, EKEY_ACCEPT=0x94ed438, EKEY_BACK=0x94ed450,
       GOT_ACCEPT=0x8fa4680, GOT_BACK=0x8fa4688 };
static GuestFn ekeys_init_orig; static u64 g_main_base;
static void ekeys_init(CPU* c, u64 entry){
  ekeys_init_orig(c,entry);
  u64 b=g_main_base, acc=b+EKEY_ACCEPT, back=b+EKEY_BACK, bot=b+EKEY_BOTTOM, right=b+EKEY_RIGHT;
  const char* why=NULL;
  if(!guest_span_readable(acc,0x30,8) || !guest_span_readable(bot,0x30,8) || !guest_span_readable(b+GOT_ACCEPT,16,8)) why="keys not readable";
  else if(gr64(b+GOT_ACCEPT)!=acc || gr64(b+GOT_BACK)!=back) why="GOT entries differ";
  else if(memcmp(GP(acc),GP(right),0x18) || memcmp(GP(back),GP(bot),0x18)) why="accept is not the right button";
  else if(!gr64(acc) || gr64(acc)==gr64(back)) why="key names";
  if(why){ LOG("Orbitals: accept on the bottom button: not applied (%s)\n",why); return; }
  u8 t[0x18]; memcpy(t,GP(acc),0x18); memcpy(GP(acc),GP(back),0x18); memcpy(GP(back),t,0x18);
  LOG("Orbitals: accept on the bottom button: platform accept key = bottom face button, back = right face button\n");
}
static void ekeys_install(const SwrModInfo* mod){
  u64 pc=mod->base+EKEYS_INIT;
  /* pacibsp at the entry; the two copies: ldp x8, x10, [x20]; adrp x9; add x9, x9, #0x438 / ldp x8, x10, [x19]; adrp x9; add x9, x9, #0x450 */
  static const struct { u32 off, w; } sig[]={ {0x50dc00,0xd503237f}, {0x51188c,0xa9402a88}, {0x511894,0x9110e129},
                                               {0x5118bc,0xa9402a68}, {0x5118c4,0x91114129} };
  for(unsigned i=0;i<sizeof sig/sizeof sig[0];i++){
    u64 a=mod->base+sig[i].off;
    if(a<g_code_lo || a>=g_code_hi || gr32(a)!=sig[i].w){ LOG("Orbitals: accept on the bottom button: EKeys initializer not found (%x)\n",sig[i].off); return; }
  }
  GuestFn* e=&g_disp[(pc-g_code_lo)/4];
  if(!*e){ LOG("Orbitals: accept on the bottom button: EKeys initializer has no entry\n"); return; }
  g_main_base=mod->base; ekeys_init_orig=*e; *e=ekeys_init;
}
static void ei_install(const SwrModInfo* mod){
  if(!g_layout) return;
  g_icon_want= g_layout==1? 2 : 1; g_icon_base=mod->base;
  icon_install(mod);
  icon_prompt_install(mod);
  if(!g_accept_bottom) return;
  ekeys_install(mod);
  g_ei_vtable=mod->base+EI_PLATFORM_DATA_VTABLE;
  ei_wrap_vtable(g_ei_vtable,mod->base,mod->size,g_code_lo,g_code_hi,g_disp);
  LOG("Orbitals: button layout: %d virtual functions of the input platform data watched (vtable %llx: %llx %llx, code %llx-%llx)\n",g_neiw,
      (unsigned long long)g_ei_vtable,(unsigned long long)gr64(g_ei_vtable),(unsigned long long)gr64(g_ei_vtable+8),(unsigned long long)g_code_lo,(unsigned long long)g_code_hi);
}

/* ===================================================================== gyro aiming
   The camera turns in the engine, by the angle the controller turned (no simulated stick): APlayerController::UpdateRotation, called
   by every local player controller once a frame (also without a pawn, at the title and in menus), first gets the gyro turn through
   the engine's own AddYawInput / AddPitchInput, then the frame turns by RotationInput as for the stick, which keeps working at the
   same time. A gate (gyro_gate) lets it through only while a game player controller looks through the actor it controls (its ship
   station or its character), look input not ignored, no cinematic and the shared game pause off; otherwise the turn is dropped. While
   the game is paused UpdateRotation usually does not run, and the gate is closed if it does; the turn from before a pause is
   dropped, so nothing jumps after it.
   Samples: every SDL sensor sample of the pad, integrated with its own timestamp (frame rate independent); player space yaw (GyroWiki,
   relax factor 1.41) from the gravity the accelerometer sees; tightening under gyro_tightening deg/s; automatic calibration when the pad
   lies still for 1.5 s (spread < 0.02 rad/s) and a manual one (gyro_calibrate=1, or holding the gyro button 3 s with the pad lying
   still) stored per controller model in gyro_calibration.txt. The turn goes to the player whose pad has the gyro: pad slot n is
   ControllerId n (player 1 = 0). */
enum { UPDATE_ROTATION=0x31fdf90, UPDATE_ROTATION_SUB=0x2cea090, ADD_PITCH=0x320a7c0, ADD_YAW=0x320a850,
       PC_PLAYER=0x348, LP_CONTROLLER_ID=0x100, PC_CONTROL_ROTATION=0x320 };
static const u8 orbitals_build[16] = {0x0f,0x8e,0xf5,0x2c,0xf4,0xee,0xe3,0x76,0x28,0xf3,0x66,0xa5,0x39,0xd5,0xd6,0xad};

static struct {
  int mode;            /* 0 off, 1 always, 2 aim (left trigger held) */
  float sens_x, sens_y;
  int button;          /* SDL button index for pause/toggle, -1 none */
  int button_toggle;   /* 0 = hold to pause, 1 = press to toggle */
  float tightening;    /* deg/s */
  int invert;          /* 1 horizontal, 2 vertical, 3 both */
  int calibrate;       /* 1 = calibrate at the next still moment and store it */
  int test;            /* 1 = synthetic gyro test once a player can look */
  int log;             /* 1 = once a second per pad: raw mean / spread, bias, drift after calibration */
  float test_delay;    /* gyro_test only: seconds from arming before the test may start (past loading hitches), 0-600 */
} G={0,1.5f,1.5f,20,0,3.0f,0,0,0,0,0.0f};

void orbitals_input_ini(const char* k, const char* v){
  if(!strcmp(k,"button_layout")){ g_layout= !strcmp(v,"xbox")? 1 : !strcmp(v,"playstation")? 2 : 0; }
  else if(!strcmp(k,"button_accept_bottom")) g_accept_bottom=atoi(v)==1;
  else if(!strcmp(k,"gyro")) G.mode= !strcmp(v,"always")? 1 : !strcmp(v,"aim")? 2 : 0;
  else if(!strcmp(k,"gyro_sensitivity_x")){ float f=(float)atof(v); if(f>0 && f<=20) G.sens_x=f; }
  else if(!strcmp(k,"gyro_sensitivity_y")){ float f=(float)atof(v); if(f>0 && f<=20) G.sens_y=f; }
  else if(!strcmp(k,"gyro_button")) G.button= !strcmp(v,"none")? -1 : !strcmp(v,"l3")? 7 : !strcmp(v,"r3")? 8 : !strcmp(v,"mute")? 15 :
                                             !strcmp(v,"paddle_right")? 16 : !strcmp(v,"paddle_left")? 17 : 20;
  else if(!strcmp(k,"gyro_button_mode")) G.button_toggle= !strcmp(v,"toggle");
  else if(!strcmp(k,"gyro_tightening")){ float f=(float)atof(v); if(f>=0 && f<=30) G.tightening=f; }
  else if(!strcmp(k,"gyro_invert")) G.invert=atoi(v)&3;
  else if(!strcmp(k,"gyro_calibrate")) G.calibrate=atoi(v)==1;
  else if(!strcmp(k,"gyro_test")) G.test=atoi(v)==1;
  else if(!strcmp(k,"gyro_test_delay")){ float f=(float)atof(v); if(f>=0 && f<=600) G.test_delay=f; }
  else if(!strcmp(k,"gyro_log")) G.log=atoi(v)==1;
}
int orbitals_gyro_wanted(void){ return G.mode || G.test || G.calibrate || G.log; }

#define RAD2DEG 57.29577951308232
enum { SLOTS=2 };
typedef struct {
  u32 id; u16 vendor, product;
  float bias[3]; int calibrated;           /* 1 auto, 2 stored/manual */
  float grav[3]; int grav_ok;              /* low-passed accelerometer (points up at rest) */
  u64 last_t;                              /* timestamp of the previous sample */
  u64 still_t0, still_last; int still_n; double still_sum[3]; float still_min[3], still_max[3];
  int toggled_off, button_was, has_gyro; u64 button_t0;   /* button_t0: plat_ns() when the gyro button went down */
  int logged_samples;
  u64 log_t0; int log_n; double log_sum[3]; float log_min[3], log_max[3]; double drift[3];   /* gyro_log */
} GyroPad;
static GyroPad P[SLOTS];

/* the turn waiting for the player's next UpdateRotation, degrees; written by the frontend thread, taken by the game thread */
static struct { int lock; double yaw, pitch; u64 taken_ns; } Acc[SLOTS];
static void acc_lock(int s){ while(__atomic_exchange_n(&Acc[s].lock,1,__ATOMIC_ACQUIRE)) {} }
static void acc_unlock(int s){ __atomic_store_n(&Acc[s].lock,0,__ATOMIC_RELEASE); }
static void acc_clear(int s){ acc_lock(s); Acc[s].yaw=Acc[s].pitch=0; acc_unlock(s); }

/* The launcher creates a new executable directory each run, with data/save a junction to the profile's save directory. That
   directory only holds the game's own save kinds (export, backup and restore refuse anything else, and a restore replaces it), so
   the calibration lives one level up: the profile directory (a standalone install: Orbitals_data). The junction is resolved to the
   real directory first, so a fresh session finds it and other profiles stay isolated; any failure stores nothing. */
#ifdef _WIN32
typedef wchar_t CalibChar;
#define CALIB_N 4096
static int calib_path(CalibChar* p, size_t n, int tmp){
  char save[1200]; wchar_t wsave[1200];
  if((size_t)snprintf(save,sizeof save,"%ssave",g_datadir)>=sizeof save) return 0;
  plat_mkdir(save);
  if(!MultiByteToWideChar(CP_ACP,MB_ERR_INVALID_CHARS,save,-1,wsave,1200)) return 0;
  HANDLE h=CreateFileW(wsave,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
  if(h==INVALID_HANDLE_VALUE) return 0;
  BY_HANDLE_FILE_INFORMATION info; DWORD len=0;
  if(GetFileInformationByHandle(h,&info) && (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))
    len=GetFinalPathNameByHandleW(h,p,(DWORD)n,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
  CloseHandle(h);
  if(!len || len>=n) return 0;                      /* failed or truncated */
  wchar_t* name=wcsrchr(p,L'\\');
  if(!name || _wcsicmp(name+1,L"save")) return 0;   /* not the save directory the launcher or the install made */
  *name=0;
  if(!wcsrchr(p,L'\\') || name[-1]==L':') return 0;   /* never a volume root */
  const wchar_t* file= tmp? L"\\gyro_calibration.txt.tmp" : L"\\gyro_calibration.txt";
  size_t used=(size_t)(name-p), add=wcslen(file);
  if(used+add>=n) return 0;
  memcpy(name,file,(add+1)*sizeof *file); return 1;
}
static FILE* calib_open(const CalibChar* p, int write){ return _wfopen(p,write? L"wb" : L"rb"); }
static int calib_replace(const CalibChar* tmp, const CalibChar* p){ return MoveFileExW(tmp,p,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0; }
#else
typedef char CalibChar;
#define CALIB_N 4096
static int calib_path(CalibChar* p, size_t n, int tmp){
  char save[1200];
  if((size_t)snprintf(save,sizeof save,"%ssave",g_datadir)>=sizeof save) return 0;
  plat_mkdir(save);
  char* real=realpath(save,NULL); if(!real) return 0;
  char* name=strrchr(real,'/');
  int ok= name && name!=real && !strcmp(name+1,"save");   /* the save directory, and its parent is not / */
  if(ok){ *name=0; size_t w=(size_t)snprintf(p,n,"%s/gyro_calibration.txt%s",real,tmp? ".tmp" : ""); ok= w<n; }
  free(real); return ok;
}
static FILE* calib_open(const CalibChar* p, int write){ return fopen(p,write? "wb" : "rb"); }
static int calib_replace(const CalibChar* tmp, const CalibChar* p){ return rename(tmp,p)==0; }
#endif
static int calib_load(GyroPad* g){
  CalibChar p[CALIB_N]; if(!calib_path(p,CALIB_N,0)) return 0;
  FILE* f=calib_open(p,0); if(!f) return 0;
  char line[256]; int found=0;
  while(fgets(line,sizeof line,f)){
    unsigned vid,pid; float b[3];
    if(sscanf(line,"%x:%x %f %f %f",&vid,&pid,&b[0],&b[1],&b[2])==5 && vid==g->vendor && pid==g->product){ memcpy(g->bias,b,sizeof b); found=1; }
  }
  fclose(f); return found;
}
static int calib_store(const GyroPad* g){
  CalibChar p[CALIB_N], tmp[CALIB_N];
  if(!calib_path(p,CALIB_N,0) || !calib_path(tmp,CALIB_N,1)) return 0;
  FILE* in=calib_open(p,0); FILE* out=calib_open(tmp,1); if(!out){ if(in) fclose(in); return 0; }
  if(in){ char line[256]; while(fgets(line,sizeof line,in)){ unsigned vid,pid; if(sscanf(line,"%x:%x",&vid,&pid)==2 && vid==g->vendor && pid==g->product) continue; fputs(line,out); } fclose(in); }
  fprintf(out,"%04x:%04x %.6f %.6f %.6f\n",g->vendor,g->product,g->bias[0],g->bias[1],g->bias[2]);
  int failed=ferror(out); if(fclose(out)) failed=1;
  return !failed && calib_replace(tmp,p);
}

static void still_reset(GyroPad* g, u64 t, const float* raw){
  g->still_t0=t; g->still_n=0;
  for(int k=0;k<3;k++){ g->still_sum[k]=0; g->still_min[k]=g->still_max[k]=raw[k]; }
}
/* calibration: a window where every axis stays within 0.02 rad/s of itself and under 0.04 rad/s (2.3 deg/s: a pad's offset is
   well under that, so a slow steady turn is not taken for one). The automatic one never moves a stored calibration by more than
   0.015 rad/s. Manual (gyro_calibrate=1, or the gyro button held): a 2 s / 3 s window that starts once asked */
static void still_sample(int s, GyroPad* g, u64 t, const float* raw, int manual_hold){
  /* The sample clock can switch between the host's poll time and the device's own sensor timestamps, either way (the first SDL
     sensor event; polling again after 500 ms without events). A sample that goes back, or comes over 50 ms after the previous one
     (the gap gyro_sample never integrates across), starts a fresh still window: the window only measures time the pad was sampled. */
  u64 last=g->still_last; g->still_last=t;
  if(t<last || t-last>50000000ull){ still_reset(g,t,raw); return; }
  int moving=0;
  for(int k=0;k<3;k++){
    if(fabsf(raw[k])>0.04f) moving=1;
    if(raw[k]<g->still_min[k]) g->still_min[k]=raw[k];
    if(raw[k]>g->still_max[k]) g->still_max[k]=raw[k];
    if(g->still_max[k]-g->still_min[k]>0.02f) moving=1;
  }
  if(moving || !g->still_t0){ still_reset(g,t,raw); return; }
  for(int k=0;k<3;k++) g->still_sum[k]+=raw[k];
  g->still_n++;
  u64 need= manual_hold? 3000000000ull : G.calibrate? 2000000000ull : 1500000000ull;
  if(t<g->still_t0 || t-g->still_t0<need || g->still_n<50) return;
  float mean[3]; for(int k=0;k<3;k++) mean[k]=(float)(g->still_sum[k]/g->still_n);
  if(!(G.calibrate || manual_hold) && g->calibrated==2)
    for(int k=0;k<3;k++) if(fabsf(mean[k]-g->bias[k])>0.015f){ still_reset(g,t,raw); return; }
  memcpy(g->bias,mean,sizeof mean);
  if(G.calibrate || manual_hold){
    if(calib_store(g)){
      g->calibrated=2;
      LOG("gyro: P%d calibrated and stored for %04x:%04x: %.4f %.4f %.4f rad/s (%d samples)\n",s+1,g->vendor,g->product,g->bias[0],g->bias[1],g->bias[2],g->still_n);
      if(G.calibrate){ G.calibrate=0; ini_set("gyro_calibrate","0"); }
    } else {
      g->calibrated=1;
      WARN("gyro: P%d calibration active for this session, but could not store it in the profile\n",s+1);
    }
  } else {
    if(!g->calibrated) LOG("gyro: P%d auto calibration %.4f %.4f %.4f rad/s\n",s+1,g->bias[0],g->bias[1],g->bias[2]);
    if(!g->calibrated) g->calibrated=1;
  }
  still_reset(g,t,raw);
}

/* the synthetic test: phase 1 a 90 deg/s left turn for 1 s, phase 2 a 30 deg/s upward tilt for 1 s, at 250 samples/s,
   through the same processing as the pad's samples (pad samples are left out meanwhile) */
static struct {
  int state;            /* 0 idle, 1 waiting for a player that can look, 2 running, 3 settling, 4 done */
  int slot; u64 pc, t0, next_sample;   /* pc: the player controller under test */
  u64 armed_ns, delay_ns;               /* plat_ns() when armed; it may start only delay_ns after that (gyro_test_delay) */
  double want_yaw, want_pitch;          /* integral x sensitivity, camera degrees (UE signs) */
  double applied_yaw, applied_pitch;    /* given to AddYawInput / AddPitchInput */
  double cam_yaw, cam_pitch;            /* ControlRotation change measured around UpdateRotation */
  int lock;
} T;   /* shared by the frontend thread (samples) and the game thread (camera): every access under test_lock */
static void test_lock(void){ while(__atomic_exchange_n(&T.lock,1,__ATOMIC_ACQUIRE)) {} }
static void test_unlock(void){ __atomic_store_n(&T.lock,0,__ATOMIC_RELEASE); }
static void test_arm(void){ u64 now=plat_ns(); test_lock(); T.state=1; T.armed_ns=now; T.delay_ns=(u64)(G.test_delay*1e9); test_unlock(); }

static void acc_add(int s, u64 now, double dyaw, double dpitch){
  acc_lock(s);
  u64 taken=__atomic_load_n(&Acc[s].taken_ns,__ATOMIC_RELAXED);   /* the game thread may have taken it after now was read */
  if(now>taken && now-taken>100000000ull){ Acc[s].yaw=0; Acc[s].pitch=0; }   /* nobody looking: drop it */
  Acc[s].yaw+=dyaw; Acc[s].pitch+=dpitch;
  acc_unlock(s);
}

static void gyro_sample(int s, GyroPad* g, const PadMotion* m, int synthetic, int active, int manual_hold){
  u64 dt_ns= g->last_t && m->t_ns>g->last_t? m->t_ns-g->last_t : 0;
  g->last_t=m->t_ns;
  if(dt_ns>50000000ull) dt_ns=0;   /* a gap (reconnect, pause): never integrate across it */
  float a[3]={m->accel[0],m->accel[1],m->accel[2]};
  float amag=sqrtf(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);
  if(amag>1.0f){
    if(!g->grav_ok){ memcpy(g->grav,a,sizeof a); g->grav_ok=1; }
    else { float k=dt_ns? (float)(dt_ns/1e9/(0.08+dt_ns/1e9)) : 0.05f; for(int i=0;i<3;i++) g->grav[i]+=(a[i]-g->grav[i])*k; }
  }
  if(!synthetic) still_sample(s,g,m->t_ns,m->gyro,manual_hold);
  if(G.log && !synthetic){   /* the still test: what the pad reports at rest, and how far it would turn the camera once calibrated */
    if(!g->log_t0 || m->t_ns<g->log_t0){ g->log_t0=m->t_ns; g->log_n=0; for(int k=0;k<3;k++){ g->log_sum[k]=0; g->log_min[k]=g->log_max[k]=m->gyro[k]; } }
    for(int k=0;k<3;k++){ g->log_sum[k]+=m->gyro[k]; if(m->gyro[k]<g->log_min[k]) g->log_min[k]=m->gyro[k]; if(m->gyro[k]>g->log_max[k]) g->log_max[k]=m->gyro[k];
      if(dt_ns && g->calibrated) g->drift[k]+=(m->gyro[k]-g->bias[k])*dt_ns/1e9*RAD2DEG; }
    g->log_n++;
    if(m->t_ns-g->log_t0>=1000000000ull){
      LOG("gyro still P%d: %d samples, mean %.4f %.4f %.4f rad/s, spread %.4f %.4f %.4f, bias %.4f %.4f %.4f (%s), drift since calibrated %.3f %.3f %.3f deg\n",
          s+1,g->log_n,g->log_sum[0]/g->log_n,g->log_sum[1]/g->log_n,g->log_sum[2]/g->log_n,g->log_max[0]-g->log_min[0],g->log_max[1]-g->log_min[1],g->log_max[2]-g->log_min[2],
          g->bias[0],g->bias[1],g->bias[2],g->calibrated==2? "stored" : g->calibrated? "auto" : "none",g->drift[0],g->drift[1],g->drift[2]);
      g->log_t0=0;
    }
  }
  if(!dt_ns || !active) return;
  float w[3]; for(int i=0;i<3;i++) w[i]=m->gyro[i]-(synthetic? 0.0f : g->bias[i]);
  /* player space: yaw is the turn around the world's up axis, the controller's pitch axis gives pitch */
  float gn[3]={0,-1,0};
  if(g->grav_ok){ float n=sqrtf(g->grav[0]*g->grav[0]+g->grav[1]*g->grav[1]+g->grav[2]*g->grav[2]); if(n>0.5f) for(int i=0;i<3;i++) gn[i]=-g->grav[i]/n; }
  float world_yaw=-(gn[1]*w[1]+gn[2]*w[2]);
  float yz=sqrtf(w[1]*w[1]+w[2]*w[2]);
  float yaw=fminf(fabsf(world_yaw)*1.41f,yz); if(world_yaw<0) yaw=-yaw;
  float pitch=w[0];
  /* rad/s -> deg/s, tightening below the threshold */
  double yd=yaw*RAD2DEG, pd=pitch*RAD2DEG, mag=sqrt(yd*yd+pd*pd);
  if(G.tightening>0 && mag<G.tightening && !synthetic){ double k=mag/G.tightening; yd*=k; pd*=k; }
  double dt=dt_ns/1e9;
  /* Unreal: yaw grows to the right, pitch grows upward; a left turn of the pad (+Y) turns the camera left */
  double dyaw=-yd*dt*G.sens_x, dpitch=pd*dt*G.sens_y;
  if(G.invert&1) dyaw=-dyaw;
  if(G.invert&2) dpitch=-dpitch;
  u64 now=plat_ns();   /* read before any lock: no clock read ever happens under test_lock or an Acc lock */
  if(!synthetic){ acc_add(s,now,dyaw,dpitch); return; }
  /* a synthetic sample is counted and published in one test_lock section (lock order test_lock -> Acc, the only nesting), and
     only while the test still runs on this slot: a game-thread end comes either before it (the sample is refused) or after it
     (test_inconclusive clears this Acc under the same lock), so an ended test never leaves a synthetic turn behind */
  test_lock();
  if(T.state==2 && T.slot==s){ T.want_yaw+=dyaw; T.want_pitch+=dpitch; acc_add(s,now,dyaw,dpitch); }
  test_unlock();
}

void orbitals_gyro_pad(int s, u32 id, u16 vendor, u16 product, u32 buttons, u16 zl, const PadMotion* m, int n, int has_gyro, int allowed){
  if(s<0 || s>=SLOTS) return;
  GyroPad* g=&P[s];
  if(g->id!=id || g->has_gyro!=has_gyro){   /* another pad, or its sensors just came up: start over (a stored calibration first) */
    memset(g,0,sizeof *g); g->id=id; g->vendor=vendor; g->product=product; g->has_gyro=has_gyro;
    acc_clear(s);
    if(id && has_gyro){
      if(calib_load(g)){ g->calibrated=2; LOG("gyro: P%d %04x:%04x, stored calibration %.4f %.4f %.4f rad/s\n",s+1,vendor,product,g->bias[0],g->bias[1],g->bias[2]); }
      else LOG("gyro: P%d %04x:%04x has a gyro (calibrates when it lies still)\n",s+1,vendor,product);
    }
  }
  /* the gyro button: hold = pause (or press = toggle); held 3 s with the pad lying still = manual calibration */
  int held= G.button>=0 && (buttons>>G.button&1);
  u64 now=plat_ns();
  if(held && !g->button_was){ g->button_t0=now; g->still_t0=0; if(G.button_toggle) g->toggled_off=!g->toggled_off; }   /* the window starts at the press */
  if(!held) g->button_t0=0;
  g->button_was=held;
  int manual= held && g->button_t0 && now-g->button_t0<20000000000ull;
  /* allowed: the game window has the focus and the PC settings panel is closed, as for the buttons */
  int active= allowed && G.mode && !(held && !G.button_toggle) && !(G.button_toggle && g->toggled_off) && (G.mode!=2 || zl>7710);
  /* test: synthetic samples on the slot of the player under test, the pad's own samples are left out */
  test_lock();
  int testing= T.state==2 && T.slot==s, ended=0;
  if(testing){
    if(!T.next_sample){ T.next_sample=now; T.t0=now; g->last_t=0; }
    for(;T.next_sample<=now;T.next_sample+=4000000ull){
      /* the lock is let go around gyro_sample: the game thread may have ended the test meanwhile (INCONCLUSIVE); that end stays */
      if(T.state!=2 || T.slot!=s){ ended=1; break; }
      u64 since=T.next_sample-T.t0;
      PadMotion sm={T.next_sample,{0,0,0},{0,9.80665f,0}};
      if(since<1000000000ull) sm.gyro[1]=(float)(90.0/RAD2DEG);
      else if(since<2000000000ull) sm.gyro[0]=(float)(30.0/RAD2DEG);
      else { T.t0=now; T.state=3; break; }
      test_unlock(); gyro_sample(s,g,&sm,1,1,0); test_lock();
    }
  }
  test_unlock();
  if(ended) acc_clear(s);   /* the synthetic turn of an ended test never reaches the camera */
  if(testing) return;
  if(!allowed) acc_clear(s);
  if(!has_gyro) return;
  for(int i=0;i<n;i++){
    gyro_sample(s,g,&m[i],0,active,manual);
    if(g->logged_samples<3 && G.mode){ g->logged_samples++;
      LOG("gyro: P%d sample t=%llu gyro %.4f %.4f %.4f accel %.3f %.3f %.3f\n",s+1,(unsigned long long)m[i].t_ns,m[i].gyro[0],m[i].gyro[1],m[i].gyro[2],m[i].accel[0],m[i].accel[1],m[i].accel[2]); }
  }
}

/* --- the game side: UpdateRotation of a local player controller */
static GuestFn rot_orig, rot_sub_orig;
static u64 g_add_yaw, g_add_pitch;

static void guest_call_float(CPU* c, u64 fn, u64 obj, float v){
  GuestFn f=g_disp[(fn-g_code_lo)/4];
  if(!f) return;
  u64 lr=c->x[30];
  c->x[0]=obj; memset(&c->v[0],0,sizeof c->v[0]); c->v[0].f[0]=v;
  c->x[30]=fn; f(c,fn);   /* returns with c->pc = the x30 given */
  c->x[30]=lr;
}

static int pc_controller_id(u64 pc){
  if(!guest_span_readable(pc+PC_PLAYER,8,8)) return -1;
  u64 lp=gr64(pc+PC_PLAYER);
  if(!lp || !guest_span_readable(lp+LP_CONTROLLER_ID,4,4)) return -1;
  return (int)gr32(lp+LP_CONTROLLER_ID);
}
static double wrap180(double d){ while(d>180) d-=360; while(d<-180) d+=360; return d; }

/* The gate: the gyro turns a player controller only while that player plainly looks through the actor it controls itself.
   UpdateRotation also runs without one (PlayerTick calls it while the controller is Inactive: the title and menus), so its call
   alone proves nothing, and in this game the camera of a player at the helm is a separate controlled actor, not the pawn (PC run,
   docs/dev4k/GYRO.md). Read before the game's UpdateRotation runs (offsets: offline RE of main, checked against live snapshots):
   - the controller is an OrbBasePlayerController (native IsA on the existing class singleton; the title's OrbFrontEndPlayerController
     is 0x750 bytes, so none of the game fields below is read before this holds);
   - a pawn (an untagged pointer) that the controller acknowledged, look input not ignored, cinematic mode (+0x4c0 bit 0x02) off;
   - the game's GetControlledActor (main+0x45144c0): ControlledActor (+0x7c8) if set, else MainCharacter (+0x7b8, required). A
     ControlledActor other than MainCharacter must be an OrbControllableActor whose controlling player controller (+0x2f0) is this one;
   - the camera manager's view target is that actor and it is not blending to another actor;
   - the shared pause of the world's game state is off: the controller's Outer chain (bounded, cycle checked) to its ULevel, the
     level's OwningWorld (+0xc0), the world's GameState (+0x1d8), an OBGameStateBase whose pause byte (+0x42b) is 0. The pause menu
     pointer (+0x880) is never used: only the player who paused has one, and it stays cached after the game resumes;
   - a frame that moves (DeltaTime > 0).
   Anything unreadable, malformed or of an unexpected type closes it. A closed gate still takes the turn waiting for the player and
   drops it, so nothing builds up meanwhile. */
enum { PC_PAWN=0x2e8, PC_IGNORE_LOOK=0x33a, PC_ACK_PAWN=0x358, PC_CAMERA_MANAGER=0x368, PCM_VIEW_TARGET=0x340, PCM_PENDING_VIEW_TARGET=0xc00,
       OBJ_CLASS=0x10, OBJ_OUTER=0x20, CLASS_BASE_CHAIN=0x30, CLASS_BASE_DEPTH=0x38,
       PC_CINEMATIC=0x4c0, PC_CINEMATIC_BIT=0x02, PC_MAIN_CHARACTER=0x7b8, PC_CONTROLLED_ACTOR=0x7c8, CA_CONTROLLER=0x2f0,
       LEVEL_WORLD=0xc0, WORLD_GAME_STATE=0x1d8, GS_PAUSED=0x42b, GYRO_MAX_DEPTH=256, GYRO_MAX_OUTERS=32 };
/* the class singletons (main module data, filled by the engine's StaticClass; only read here, never initialized) */
enum { CLS_PLAYER_CONTROLLER, CLS_CONTROLLABLE_ACTOR, CLS_LEVEL, CLS_GAME_STATE, CLS_N };
static const u32 cls_global[CLS_N]={0x9cb95d0,0x9cba540,0x9c51c88,0x9cb7bf0};   /* OrbBasePlayerController, OrbControllableActor, ULevel, OBGameStateBase */
static u64 g_cls_slot[CLS_N];   /* their addresses in guest memory; 0 = gyro gate always closed */
enum { GATE_OPEN, GATE_UNREADABLE, GATE_NOT_GAME_CONTROLLER, GATE_NO_PAWN, GATE_TAGGED, GATE_NOT_ACKNOWLEDGED, GATE_IGNORE_LOOK,
       GATE_CINEMATIC, GATE_NO_MAIN_CHARACTER, GATE_CONTROLLED_ACTOR, GATE_WRONG_OWNER, GATE_NO_CAMERA, GATE_VIEW_TARGET,
       GATE_PENDING_VIEW_TARGET, GATE_NO_GAME_STATE, GATE_PAUSED, GATE_NO_TIME };
static const char* const gate_reason[]={"open","controller unreadable","not a game player controller","no pawn","tagged pawn pointer",
  "pawn not acknowledged","look input ignored","cinematic mode","no main character","controlled actor not a controllable actor",
  "controlled actor has another controller","camera manager missing or unreadable","view target is not the controlled actor",
  "blending to another view target","game state not found","game paused","DeltaTime 0"};
typedef struct { u64 pawn, ack, pcm, vt, pending, main, controlled, owner, gs; int ignore, cinematic, paused; } GateSeen;

/* a plain object pointer: set, 8 aligned, no tag or high bits */
static int gyro_ptr(u64 p){ return p && !(p&7) && p < (1ull<<48); }
/* a field of an object: the base itself must be a plain pointer before any offset is added (a null or wrapping base never reaches
   a readable address by its offset) */
static int gyro_field64(u64 base, u64 off, u64* v){
  if(!gyro_ptr(base) || off>=(1ull<<32) || !guest_span_readable(base+off,8,8)) return 0;
  *v=gr64(base+off); return 1;
}
/* the engine's UObject IsA (main+0x451d700): the object's class (+0x10) has the wanted class in its base chain: the chain array
   (+0x30) entry at the wanted class's depth (+0x38, signed) is the wanted class's own chain struct (wanted class + 0x30, not the class).
   The wanted class's own chain must name itself at its depth too, so a stale or wrong singleton never matches.
   1 = is one, 0 = a well-formed object of another class, -1 = the object, its class or the wanted class is invalid or unreadable. */
static int gyro_class_depth(u64 cls, u64* chain, i32* depth){
  if(!gyro_ptr(cls) || !guest_span_readable(cls+CLASS_BASE_CHAIN,16,8)) return 0;
  *chain=gr64(cls+CLASS_BASE_CHAIN); *depth=(i32)gr32(cls+CLASS_BASE_DEPTH);
  return gyro_ptr(*chain) && *depth>=0 && *depth<=GYRO_MAX_DEPTH;
}
static int gyro_isa3(u64 obj, int which){
  u64 want, cls, wchain, chain, entry; i32 wd, d;
  if(!g_cls_slot[which] || !gyro_field64(g_cls_slot[which],0,&want) || !gyro_class_depth(want,&wchain,&wd)) return -1;
  if(!gyro_field64(wchain,(u64)wd*8,&entry) || entry!=want+CLASS_BASE_CHAIN) return -1;
  if(!gyro_field64(obj,OBJ_CLASS,&cls) || !gyro_class_depth(cls,&chain,&d)) return -1;
  /* the object's class must name itself at its own depth (read only up to that depth) before it counts as a well-formed class */
  if(!gyro_field64(chain,(u64)d*8,&entry) || entry!=cls+CLASS_BASE_CHAIN) return -1;
  if(wd>d) return 0;   /* shallower than the wanted class: another class (its chain is not read past its own depth) */
  if(!gyro_field64(chain,(u64)wd*8,&entry)) return -1;
  return entry==want+CLASS_BASE_CHAIN;
}
static int gyro_isa(u64 obj, int which){ return gyro_isa3(obj,which)==1; }
/* the shared pause: 1 paused, 0 running, -1 not found. Like AActor::GetWorld (GetTypedOuter<ULevel>, then OwningWorld). */
static int gyro_paused(u64 pc, u64* gs_out){
  u64 seen[GYRO_MAX_OUTERS], o, level=0, world, gs;
  if(!gyro_field64(pc,OBJ_OUTER,&o)) return -1;
  for(int n=0;n<GYRO_MAX_OUTERS && !level;n++){
    if(!gyro_ptr(o)) return -1;
    for(int k=0;k<n;k++) if(seen[k]==o) return -1;   /* a cycle */
    seen[n]=o;
    int is=gyro_isa3(o,CLS_LEVEL);
    if(is<0) return -1;   /* an Outer with invalid class metadata is not skipped as "not a level" */
    if(is) level=o;
    else if(!gyro_field64(o,OBJ_OUTER,&o)) return -1;
  }
  if(!level || !gyro_field64(level,LEVEL_WORLD,&world) || !gyro_field64(world,WORLD_GAME_STATE,&gs)) return -1;
  *gs_out=gs;
  if(!gyro_isa(gs,CLS_GAME_STATE) || !guest_span_readable(gs+GS_PAUSED,1,1)) return -1;   /* gs checked as a plain pointer by IsA */
  return *GP(gs+GS_PAUSED)!=0;
}
static int gyro_gate(u64 pc, float dt, GateSeen* q){
  memset(q,0,sizeof *q);
  if(!guest_span_readable(pc+PC_PAWN,8,8) || !guest_span_readable(pc+PC_IGNORE_LOOK,1,1) || !guest_span_readable(pc+PC_ACK_PAWN,8,8)
     || !guest_span_readable(pc+PC_CAMERA_MANAGER,8,8)) return GATE_UNREADABLE;
  q->pawn=gr64(pc+PC_PAWN); q->ack=gr64(pc+PC_ACK_PAWN); q->ignore=*GP(pc+PC_IGNORE_LOOK); q->pcm=gr64(pc+PC_CAMERA_MANAGER);
  int pcm_ok= gyro_ptr(q->pcm) && guest_span_readable(q->pcm+PCM_VIEW_TARGET,8,8) && guest_span_readable(q->pcm+PCM_PENDING_VIEW_TARGET,8,8);
  if(pcm_ok){ q->vt=gr64(q->pcm+PCM_VIEW_TARGET); q->pending=gr64(q->pcm+PCM_PENDING_VIEW_TARGET); }
  if(!gyro_isa(pc,CLS_PLAYER_CONTROLLER)) return GATE_NOT_GAME_CONTROLLER;   /* before any game field */
  if(!guest_span_readable(pc+PC_CINEMATIC,1,1) || !guest_span_readable(pc+PC_MAIN_CHARACTER,8,8)
     || !guest_span_readable(pc+PC_CONTROLLED_ACTOR,8,8)) return GATE_UNREADABLE;
  q->cinematic=*GP(pc+PC_CINEMATIC); q->main=gr64(pc+PC_MAIN_CHARACTER); q->controlled=gr64(pc+PC_CONTROLLED_ACTOR);
  if(!q->pawn) return GATE_NO_PAWN;
  if(!gyro_ptr(q->pawn)) return GATE_TAGGED;
  if(q->ack!=q->pawn) return GATE_NOT_ACKNOWLEDGED;
  if(q->ignore) return GATE_IGNORE_LOOK;
  if(q->cinematic&PC_CINEMATIC_BIT) return GATE_CINEMATIC;
  if(!gyro_ptr(q->main)) return GATE_NO_MAIN_CHARACTER;
  u64 actor= q->controlled? q->controlled : q->main;   /* GetControlledActor */
  if(actor!=q->main){
    if(!gyro_isa(actor,CLS_CONTROLLABLE_ACTOR)) return GATE_CONTROLLED_ACTOR;   /* before its controller field */
    if(!gyro_field64(actor,CA_CONTROLLER,&q->owner)) return GATE_CONTROLLED_ACTOR;
    if(q->owner!=pc) return GATE_WRONG_OWNER;
  }
  if(!pcm_ok) return GATE_NO_CAMERA;
  if(q->vt!=actor) return GATE_VIEW_TARGET;
  if(q->pending && q->pending!=actor) return GATE_PENDING_VIEW_TARGET;
  q->paused=gyro_paused(pc,&q->gs);
  if(q->paused<0) return GATE_NO_GAME_STATE;
  if(q->paused) return GATE_PAUSED;
  if(!(dt>0)) return GATE_NO_TIME;
  return GATE_OPEN;
}
/* per player, game thread only: the gate seen last, the previous UpdateRotation, (gyro_log) the turn dropped while closed
   and since when its gate has been open without a break (no close, no UpdateRotation gap over 100 ms, the same controller) */
static struct { u64 pc; int reason, logged; u64 last_ns, open_since, drop_t0; double drop_yaw, drop_pitch; } Gs[SLOTS];

static void test_inconclusive(const char* why){   /* under test_lock; the waiting turn of a running test is dropped (test_lock -> Acc) */
  if(T.state==2 || T.state==3) acc_clear(T.slot);
  T.state=4; LOG("gyro test: INCONCLUSIVE (%s); no result\n",why);
}

static void update_rotation(CPU* c, u64 entry, GuestFn orig){
  u64 pc=c->x[0]; float dt=c->v[0].f[0];
  int id= (G.mode || T.state)? pc_controller_id(pc) : -1;
  if(id<0 || id>=SLOTS){ orig(c,entry); return; }
  static u64 seen[SLOTS];
  if(seen[id]!=pc){ seen[id]=pc; LOG("gyro: player controller %llx has ControllerId %d\n",(unsigned long long)pc,id); }
  GateSeen q; int reason=gyro_gate(pc,dt,&q), open= reason==GATE_OPEN;
  u64 now=plat_ns();
  if(G.log && (!Gs[id].logged || Gs[id].pc!=pc || Gs[id].reason!=reason))
    LOG("gyro gate: P%d controller %llx %s (%s): pawn %llx acknowledged %llx main %llx controlled %llx its controller %llx view target %llx"
        " pending %llx ignore look %d cinematic %02x game state %llx paused %d dt %.4f\n",
        id+1,(unsigned long long)pc,open? "open" : "closed",gate_reason[reason],(unsigned long long)q.pawn,(unsigned long long)q.ack,
        (unsigned long long)q.main,(unsigned long long)q.controlled,(unsigned long long)q.owner,(unsigned long long)q.vt,
        (unsigned long long)q.pending,q.ignore,q.cinematic,(unsigned long long)q.gs,q.paused,dt);
  /* UpdateRotation stops while the game is paused (the paused tick skips PlayerTick): the turn waiting from before such a gap is
     dropped, as gyro_sample already drops one nobody took for 100 ms */
  u64 gap_ns= Gs[id].last_ns? now-Gs[id].last_ns : 0;
  int gap= gap_ns>100000000ull;
  if(G.log && gap_ns>200000000ull) LOG("gyro gate: P%d UpdateRotation again after %.0f ms\n",id+1,gap_ns/1e6);
  if(!open) Gs[id].open_since=0;
  else if(!Gs[id].open_since || gap || Gs[id].pc!=pc) Gs[id].open_since=now;
  Gs[id].pc=pc; Gs[id].reason=reason; Gs[id].logged=1; Gs[id].last_ns=now;
  int ended=0;
  test_lock();
  /* past the gyro_test_delay, then 3 s of unbroken open gate */
  if(T.state==1 && now>=T.armed_ns+T.delay_ns && (G.mode==0 || id==T.slot) && Gs[id].open_since && now-Gs[id].open_since>3000000000ull){
    T.slot=id; T.pc=pc; T.state=2; LOG("gyro test: start on ControllerId %d, %.2f s after arming\n",id,(now-T.armed_ns)/1e9);
  }
  int measure= T.slot==id && (T.state==2 || T.state==3);
  if(measure){   /* any break once it runs ends it INCONCLUSIVE, for good */
    char why[96]="";
    if(pc!=T.pc) snprintf(why,sizeof why,"another player controller took this player");
    else if(gap) snprintf(why,sizeof why,"no UpdateRotation for %.0f ms during the test",gap_ns/1e6);
    else if(!open) snprintf(why,sizeof why,"the gate closed during the test: %s",gate_reason[reason]);
    else if(!guest_span_readable(pc+PC_CONTROL_ROTATION,16,8)) snprintf(why,sizeof why,"ControlRotation unreadable");
    if(why[0]){ test_inconclusive(why); measure=0; ended=1; }
  }
  test_unlock();
  double yaw=0, pitch=0;
  acc_lock(id); yaw=Acc[id].yaw; pitch=Acc[id].pitch; Acc[id].yaw=Acc[id].pitch=0; __atomic_store_n(&Acc[id].taken_ns,now,__ATOMIC_RELAXED); acc_unlock(id);
  if(!open || gap || ended){   /* dropped, never kept for later (nor the synthetic turn of an ended test) */
    if(G.log && (yaw || pitch)){
      if(!Gs[id].drop_t0) Gs[id].drop_t0=now;
      Gs[id].drop_yaw+=fabs(yaw); Gs[id].drop_pitch+=fabs(pitch);
    }
    yaw=pitch=0;
  }
  if(G.log && Gs[id].drop_t0 && now-Gs[id].drop_t0>=1000000000ull){
    LOG("gyro gate: P%d dropped %.3f deg yaw, %.3f deg pitch in %.1f s (%s)\n",id+1,Gs[id].drop_yaw,Gs[id].drop_pitch,(now-Gs[id].drop_t0)/1e9,gate_reason[reason]);
    Gs[id].drop_t0=0; Gs[id].drop_yaw=Gs[id].drop_pitch=0;
  }
  double cp0=0, cy0=0;
  if(measure){ cp0=*(double*)GP(pc+PC_CONTROL_ROTATION); cy0=*(double*)GP(pc+PC_CONTROL_ROTATION+8); }
  if(yaw || pitch){
    u64 x0=c->x[0]; V128 v0=c->v[0];
    if(yaw) guest_call_float(c,g_add_yaw,pc,(float)yaw);
    if(pitch) guest_call_float(c,g_add_pitch,pc,(float)pitch);
    c->x[0]=x0; c->v[0]=v0;
    if(measure){ test_lock(); T.applied_yaw+=yaw; T.applied_pitch+=pitch; test_unlock(); }
  }
  orig(c,entry);
  if(!measure) return;
  u64 after=plat_ns();
  test_lock();
  if(T.state!=2 && T.state!=3){ test_unlock(); return; }   /* ended inside (a nested UpdateRotation) */
  if(!guest_span_readable(pc+PC_CONTROL_ROTATION,16,8)){ test_inconclusive("ControlRotation unreadable after UpdateRotation"); test_unlock(); return; }
  double dp=wrap180(*(double*)GP(pc+PC_CONTROL_ROTATION)-cp0), dy=wrap180(*(double*)GP(pc+PC_CONTROL_ROTATION+8)-cy0);
  T.cam_pitch+=dp; T.cam_yaw+=dy;
  if(T.state==3 && after>T.t0 && after-T.t0>300000000ull){
    T.state=4;
    double ey= T.want_yaw? 100.0*(T.cam_yaw-T.want_yaw)/T.want_yaw : 0, ep= T.want_pitch? 100.0*(T.cam_pitch-T.want_pitch)/T.want_pitch : 0;
    LOG("gyro test: yaw integral x sensitivity %.3f deg, applied %.3f, camera turned %.3f (%+.2f%%) -> %s\n",T.want_yaw,T.applied_yaw,T.cam_yaw,ey,fabs(ey)<=2? "PASS" : "FAIL");
    LOG("gyro test: pitch integral x sensitivity %.3f deg, applied %.3f, camera turned %.3f (%+.2f%%) -> %s\n",T.want_pitch,T.applied_pitch,T.cam_pitch,ep,fabs(ep)<=2? "PASS" : "FAIL");
  }
  test_unlock();
}
static void update_rotation_base(CPU* c, u64 entry){ update_rotation(c,entry,rot_orig); }
static void update_rotation_sub(CPU* c, u64 entry){ update_rotation(c,entry,rot_sub_orig); }

static int bind(const SwrModInfo* mod, u64 off, u32 w0, GuestFn hook, GuestFn* orig){
  u64 pc=mod->base+off;
  if(pc<g_code_lo || pc>=g_code_hi || gr32(pc)!=w0) return 0;
  GuestFn* e=&g_disp[(pc-g_code_lo)/4];
  if(!*e) return 0;
  *orig=*e; *e=hook; return 1;
}

void orbitals_input_install(const SwrModInfo* mod){
  if(memcmp(mod->build_id,orbitals_build,sizeof orbitals_build)) return;
  ei_install(mod);
  g_add_yaw=mod->base+ADD_YAW; g_add_pitch=mod->base+ADD_PITCH;
  for(int i=0;i<CLS_N;i++) g_cls_slot[i]=mod->base+cls_global[i];
  /* AddPitchInput / AddYawInput start with pacibsp; str d8, [sp, #-0x30]!; UpdateRotation with pacibsp; sub sp, sp, #0x90 and the
     game's override with ldrb w8, [x0, #0x73c] */
  if(gr32(g_add_yaw)!=0xd503237fu || gr32(g_add_yaw+4)!=0xfc1d0fe8u || gr32(g_add_pitch+4)!=0xfc1d0fe8u
     || !g_disp[(g_add_yaw-g_code_lo)/4] || !g_disp[(g_add_pitch-g_code_lo)/4]){ LOG("gyro: camera functions not found, gyro unavailable\n"); return; }
  int a=bind(mod,UPDATE_ROTATION,0xd503237fu,update_rotation_base,&rot_orig);
  int b=bind(mod,UPDATE_ROTATION_SUB,0x395cf008u,update_rotation_sub,&rot_sub_orig);
  char armed[64]="";
  if(G.test){ test_arm(); snprintf(armed,sizeof armed,", synthetic test armed (start delay %.1f s)",G.test_delay); }
  LOG("gyro: bound to the camera (UpdateRotation %s, override %s)%s\n",a?"yes":"no",b?"yes":"no",armed);
}
