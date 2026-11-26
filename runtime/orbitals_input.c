/* Orbitals PC Recomp, based on SwitchRecomp by MZ Labs. AGPL-3.0, see LICENSE.txt and NOTICE.txt. */
/* Orbitals input extras: the button layout (Nintendo / Xbox / PlayStation) and gyro aiming.
   Everything here is off by default (button_layout=nintendo, gyro=off): the game then runs as before. */
#include "rt.h"
#include "orbitals_input.h"
#include <math.h>

void ini_set(const char* k, const char* v);   /* main.c: stores a setting and rewrites the ini */

/* ===================================================================== button layout
   Measured on the PC (2026-11-26, docs/dev4k/BUTTON_LAYOUT.md):
   - Icons: DA_PlatformInputIcons maps each key to a texture per EPlatformType (Desktop 0, Playstation 1, Xbox 2, Switch 3) and the Switch 2
     build always draws the Switch one. button_layout=xbox / playstation copies that controller's texture into the Switch entry of every
     key in memory (ei_scan_thread / icon_map_patch): every prompt shows the Xbox / DualSense button for the same key. Buttons keep their
     places, so accept stays on the right button (B on an Xbox pad, Circle on a DualSense) and the icons say so.
   - Accept on the bottom button (button_accept_bottom=1, experimental): the Switch 2 build redirects six Enhanced Input mapping contexts
     to Switch versions (BP_Switch2InputPlatformData, MappingContextRedirects) and its UI lists those Switch versions
     (MoreporkDeveloperSettings PlatformMappingContexts). The redirect data does not come from the config at run time (a user Input.ini,
     the Generated platform layer and a replaced Switch2Input.ini inside the pak were all ignored), so the runtime empties the redirect
     maps in memory as they load and gives the UI the PC contexts (user Game.ini): the lobby, the pause menu and the game then accept on
     the bottom button. CommonUI's own buttons (title screen, main menu, "Comenzar") keep the platform's accept key, the right button.
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
   pointer +0x28, count +0x30, capacity +0x34, allocation bits +0x38 (bit count +0x50), first free +0x58, free count +0x5c. A thread
   looks for such objects in the game's memory a few times after start and empties their maps (count, bits and free list say "no
   elements"; the memory stays allocated): Enhanced Input then keeps the PC version's contexts at its next rebuild. The game's own
   UI list (MoreporkDeveloperSettings PlatformMappingContexts, user Game.ini) is set to the same PC contexts so the prompts match. */
enum { EI_PLATFORM_DATA_VTABLE=0x7fecb38 };
static u64 g_ei_vtable;
int mem_block(int i, u64* addr, u64* size, u32* perm);   /* mem.c */
int mem_with_range(u64 addr, u64 len, void (*fn)(u64 addr, u64 len, void* ctx), void* ctx);
int mem_span_ok_locked(u64 addr, u64 size, u32 perm);   /* inside mem_with_range only */
#ifdef _WIN32
#include <windows.h>
static int host_committed(const void* p, size_t n){
  MEMORY_BASIC_INFORMATION mi; const u8* a=p, *e=a+n;
  while(a<e){ if(!VirtualQuery(a,&mi,sizeof mi) || mi.State!=MEM_COMMIT || (mi.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return 0; a=(const u8*)mi.BaseAddress+mi.RegionSize; }
  return 1; }
#else
static int host_committed(const void* p, size_t n){ (void)p; (void)n; return 1; }
#endif
static int ei_clear(u64 obj){
  if((obj&7) || !mem_span_ok_locked(obj,0x78,PERM_RW)) return 0;
  u32 num=gr32(obj+0x30), max=gr32(obj+0x34), bits=gr32(obj+0x50), nfree=gr32(obj+0x5c);
  if(!num || num>256 || max<num || bits<num || bits>max+64 || nfree>num) return 0;
  gw32(obj+0x50,0); gw32(obj+0x58,0xffffffffu); gw32(obj+0x5c,0); __atomic_store_n((u32*)GP(obj+0x30),0,__ATOMIC_RELEASE);
  LOG("Orbitals: button layout: %u Switch mapping context redirects taken out of the platform data at %llx\n",num-nfree,(unsigned long long)obj);
  return 1; }
/* Button icons: DA_PlatformInputIcons maps each FKey to a TMap<EPlatformType, UTexture2D*> (Desktop 0, Playstation 1, Xbox 2,
   Switch 3), and the Switch 2 game always asks for the Switch entry. Each of those maps is 4 set elements of 24 bytes (key byte,
   texture pointer at +8, hash next id at +16, hash index at +20) holding the keys 0..3 and four textures of one class: the Switch
   element gets the texture of the layout's entry, so every prompt draws that controller's icon for the same key. */
static int g_icons_done; static u64 g_tex_vtable;   /* the texture class, from the first full match */
static int icon_map_patch(u64 a, int want){
  int at[4]={-1,-1,-1,-1};
  for(int e=0;e<4;e++){
    const u8* p=GP(a+24*e); u8 k=p[0];
    if(k>3 || at[k]>=0) return 0;
    i32 nx, hi; memcpy(&nx,p+16,4); memcpy(&hi,p+20,4);
    if(nx<-1 || nx>3 || hi<0 || hi>15) return 0;
    at[k]=e;
  }
  u64 tex[4], vt=0;
  for(int k=0;k<4;k++){
    tex[k]=gr64(a+24*at[k]+8);
    if(!tex[k] || (tex[k]&7) || !mem_span_ok_locked(tex[k],8,PERM_R) || !host_committed(GP(tex[k]),8)) return 0;
    u64 v=gr64(tex[k]); if(!v || (vt && v!=vt)) return 0; vt=v;
  }
  if(vt<g_code_lo || vt>=g_code_hi+0x8000000ull) return 0;   /* a vtable in the game module */
  if(g_tex_vtable && vt!=g_tex_vtable) return 0;
  if(!g_tex_vtable){ if(tex[0]==tex[1] || tex[1]==tex[2] || tex[2]==tex[3]) return 0; g_tex_vtable=vt; LOG("Orbitals: button icons: texture class %llx\n",(unsigned long long)vt); }
  if(tex[3]==tex[want]) return 0;
  gw64(a+24*at[3]+8,tex[want]);
  return 1;
}
static void scan_chunk(u64 c, u64 n, void* ctx){
  int* cleared=ctx;
  if(!host_committed(GP(c),(size_t)n)) return;
  const u64* w=(const u64*)GP(c);
  for(u64 k=0;k+15<n/8;k++){
    if(g_accept_bottom && w[k]==g_ei_vtable) *cleared+=ei_clear(c+k*8);
    else if((u8)w[k]<=3 && (u8)w[k+3]<=3 && (u8)w[k+6]<=3 && (u8)w[k+9]<=3 &&
            ((1u<<(u8)w[k])|(1u<<(u8)w[k+3])|(1u<<(u8)w[k+6])|(1u<<(u8)w[k+9]))==15)
      g_icons_done+=icon_map_patch(c+k*8,g_layout==1? 2 : 1);
  }
}
static void ei_scan_thread(void* arg){
  (void)arg; int cleared=0;
  u64 start=plat_ns();
  plat_thread_low_priority();
  /* the game builds new icon maps all along (about 50 a second in menus: the prompts' own copies), so the scan runs for the whole
     session, once a second (0.1-0.3 s of one low priority thread). With button_accept_bottom the redirect data loads about 16 s
     after start: from 8 to 30 s the scans then run back to back to empty it as early as possible */
  for(int round=0;!g_quit;round++){
    u64 up=(plat_ns()-start)/1000000000ull;
    plat_sleep_ns(g_accept_bottom && up>=8 && up<30? 20000000ll : 1000000000ll);
    u64 t0=plat_ns(), scanned=0;
    for(int i=0;;i++){
      u64 a,sz; u32 perm; if(!mem_block(i,&a,&sz,&perm)) break;
      if((perm&3)!=3 || (perm&4) || sz<0x1000) continue;
      for(u64 c=a;c<a+sz;c+=0x100000){   /* 1 MB at a time, under the memory lock (no unmap meanwhile), committed pages only */
        u64 n= a+sz-c<0x100000? a+sz-c : 0x100000;
        if(mem_with_range(c,n,scan_chunk,&cleared)) scanned+=n;
      }
    }
    static int said_c=-1;
    static int said_icons; if(round<2 || cleared!=said_c || (round%60==0 && g_icons_done!=said_icons)){ said_c=cleared; said_icons=g_icons_done;
      LOG("Orbitals: button layout scan %d: %.1f GB in %.1f s, %d platform data maps emptied, %d icon sets switched so far\n",round,scanned/1e9,(plat_ns()-t0)/1e9,cleared,g_icons_done); }
  }
}
/* ...and before its first use: every virtual function the object's vtable reaches (UObject's own, shared with other classes)
   goes through a wrapper that, once the call returns, empties the map of a UEnhancedInputPlatformData it was called on. The
   loader calls several of them right after reading the object's properties (post load), before Enhanced Input reads the map */
enum { EI_SLOTS=64 };
static struct { u64 pc; GuestFn orig; } g_eiw[EI_SLOTS]; static int g_neiw;
static void ei_clear_cb(u64 a, u64 n, void* ctx){ (void)n; (void)ctx; ei_clear(a); }
static void ei_virtual(CPU* c, u64 entry){
  u64 obj=c->x[0];
  GuestFn orig=NULL; for(int i=0;i<g_neiw;i++) if(g_eiw[i].pc==entry){ orig=g_eiw[i].orig; break; }
  if(!orig) return;
  orig(c,entry);
  if(obj && !(obj&7) && guest_span_readable(obj,0x78,8) && gr64(obj)==g_ei_vtable && gr32(obj+0x30)) mem_with_range(obj,0x78,ei_clear_cb,NULL);
}
static void ei_install(const SwrModInfo* mod){
  if(!g_layout) return;
  g_ei_vtable=mod->base+EI_PLATFORM_DATA_VTABLE;
  for(int i=0;g_accept_bottom && i<EI_SLOTS && g_neiw<EI_SLOTS;i++){
    u64 slot=g_ei_vtable+8ull*i; if(!guest_span_readable(slot,8,8)) break;
    u64 fn=gr64(slot); if(fn<mod->size) fn+=mod->base;   /* the game's own loader relocates its data later: still module offsets here */
    if(fn<g_code_lo || fn>=g_code_hi || (fn&3)) break;   /* past the end of the vtable */
    int dup=0; for(int k=0;k<g_neiw;k++) if(g_eiw[k].pc==fn) dup=1;
    GuestFn* e=&g_disp[(fn-g_code_lo)/4];
    if(dup || !*e || *e==ei_virtual) continue;
    g_eiw[g_neiw].pc=fn; g_eiw[g_neiw].orig=*e; g_neiw++; *e=ei_virtual;
  }
  LOG("Orbitals: button layout: %d virtual functions of the input platform data watched (vtable %llx: %llx %llx, code %llx-%llx)\n",g_neiw,
      (unsigned long long)g_ei_vtable,(unsigned long long)gr64(g_ei_vtable),(unsigned long long)gr64(g_ei_vtable+8),(unsigned long long)g_code_lo,(unsigned long long)g_code_hi);
  plat_thread(ei_scan_thread,NULL,0x10000);
}

/* ===================================================================== gyro aiming
   The camera turns in the engine, by the angle the controller turned (no simulated stick): APlayerController::UpdateRotation, called
   by every local player controller once a frame while it has a pawn to look with, first gets the gyro turn through the engine's own
   AddYawInput / AddPitchInput. Those respect IgnoreLookInput (cutscenes, menus that lock the camera), then the frame turns by
   RotationInput as for the stick, which keeps working at the same time. Nothing is added while the game is paused (DeltaTime 0).
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
} G={0,1.5f,1.5f,20,0,3.0f,0,0,0,0};

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
  u64 still_t0; int still_n; double still_sum[3]; float still_min[3], still_max[3];
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

/* The launcher creates a new executable directory each run, but data/save is a junction to the profile's persistent saves.
   Keep calibration there so a fresh session sees it, and different launcher profiles stay isolated. */
static void calib_path(char* p, size_t n){ snprintf(p,n,"%ssave/gyro_calibration.txt",g_datadir); }
static int calib_load(GyroPad* g){
  char p[1200]; calib_path(p,sizeof p); FILE* f=fopen(p,"rb"); if(!f) return 0;
  char line[256]; int found=0;
  while(fgets(line,sizeof line,f)){
    unsigned vid,pid; float b[3];
    if(sscanf(line,"%x:%x %f %f %f",&vid,&pid,&b[0],&b[1],&b[2])==5 && vid==g->vendor && pid==g->product){ memcpy(g->bias,b,sizeof b); found=1; }
  }
  fclose(f); return found;
}
static int calib_store(const GyroPad* g){
  char p[1200], tmp[1300]; calib_path(p,sizeof p); snprintf(tmp,sizeof tmp,"%s.tmp",p);
  char dir[1200]; snprintf(dir,sizeof dir,"%ssave",g_datadir); plat_mkdir(dir);
  FILE* in=fopen(p,"rb"); FILE* out=fopen(tmp,"wb"); if(!out){ if(in) fclose(in); return 0; }
  if(in){ char line[256]; while(fgets(line,sizeof line,in)){ unsigned vid,pid; if(sscanf(line,"%x:%x",&vid,&pid)==2 && vid==g->vendor && pid==g->product) continue; fputs(line,out); } fclose(in); }
  fprintf(out,"%04x:%04x %.6f %.6f %.6f\n",g->vendor,g->product,g->bias[0],g->bias[1],g->bias[2]);
  int failed=ferror(out); if(fclose(out)) failed=1;
  if(failed) return 0;
#ifdef _WIN32
  return MoveFileExA(tmp,p,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
  return rename(tmp,p)==0;
#endif
}

static void still_reset(GyroPad* g, u64 t, const float* raw){
  g->still_t0=t; g->still_n=0;
  for(int k=0;k<3;k++){ g->still_sum[k]=0; g->still_min[k]=g->still_max[k]=raw[k]; }
}
/* calibration: a window where every axis stays within 0.02 rad/s of itself and under 0.04 rad/s (2.3 deg/s: a pad's offset is
   well under that, so a slow steady turn is not taken for one). The automatic one never moves a stored calibration by more than
   0.015 rad/s. Manual (gyro_calibrate=1, or the gyro button held): a 2 s / 3 s window that starts once asked */
static void still_sample(int s, GyroPad* g, u64 t, const float* raw, int manual_hold){
  /* Polling uses the host clock; the first SDL sensor event can switch to the device's smaller timestamp.
     Start a fresh still window instead of waiting for that clock to catch up with the host's uptime. */
  if(t<g->still_t0){ still_reset(g,t,raw); return; }
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
  int slot; u64 t0, next_sample, seen_ns;
  double want_yaw, want_pitch;          /* integral x sensitivity, camera degrees (UE signs) */
  double applied_yaw, applied_pitch;    /* given to AddYawInput / AddPitchInput */
  double cam_yaw, cam_pitch;            /* ControlRotation change measured around UpdateRotation */
  int lock;
} T;   /* shared by the frontend thread (samples) and the game thread (camera): every access under test_lock */
static void test_lock(void){ while(__atomic_exchange_n(&T.lock,1,__ATOMIC_ACQUIRE)) {} }
static void test_unlock(void){ __atomic_store_n(&T.lock,0,__ATOMIC_RELEASE); }

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
  if(synthetic){ test_lock(); T.want_yaw+=dyaw; T.want_pitch+=dpitch; test_unlock(); }
  acc_lock(s);
  u64 now=plat_ns();
  if(now-__atomic_load_n(&Acc[s].taken_ns,__ATOMIC_RELAXED)>100000000ull){ Acc[s].yaw=0; Acc[s].pitch=0; }   /* nobody looking: drop it */
  Acc[s].yaw+=dyaw; Acc[s].pitch+=dpitch;
  acc_unlock(s);
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
  int testing= T.state==2 && T.slot==s;
  if(testing){
    if(!T.next_sample){ T.next_sample=now; T.t0=now; g->last_t=0; }
    for(;T.next_sample<=now;T.next_sample+=4000000ull){
      u64 since=T.next_sample-T.t0;
      PadMotion sm={T.next_sample,{0,0,0},{0,9.80665f,0}};
      if(since<1000000000ull) sm.gyro[1]=(float)(90.0/RAD2DEG);
      else if(since<2000000000ull) sm.gyro[0]=(float)(30.0/RAD2DEG);
      else { T.t0=now; T.state=3; break; }
      test_unlock(); gyro_sample(s,g,&sm,1,1,0); test_lock();
    }
  }
  test_unlock();
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

static void update_rotation(CPU* c, u64 entry, GuestFn orig){
  u64 pc=c->x[0]; float dt=c->v[0].f[0];
  int id= (G.mode || T.state)? pc_controller_id(pc) : -1;
  if(id>=0 && id<SLOTS){
    static u64 seen[SLOTS];
    if(seen[id]!=pc){ seen[id]=pc; LOG("gyro: player controller %llx has ControllerId %d\n",(unsigned long long)pc,id); }
    test_lock();
    if(T.state==1 && (G.mode==0 || id==T.slot)){ u64 now=plat_ns(); if(!T.seen_ns) T.seen_ns=now; else if(now-T.seen_ns>3000000000ull){ T.slot=id; T.state=2; LOG("gyro test: start on ControllerId %d\n",id); } }
    int measure= T.slot==id && (T.state==2 || T.state==3);
    test_unlock();
    double yaw=0, pitch=0;
    acc_lock(id); yaw=Acc[id].yaw; pitch=Acc[id].pitch; Acc[id].yaw=Acc[id].pitch=0; __atomic_store_n(&Acc[id].taken_ns,plat_ns(),__ATOMIC_RELAXED); acc_unlock(id);
    double cp0=0, cy0=0;
    if(measure){ cp0=*(double*)GP(pc+PC_CONTROL_ROTATION); cy0=*(double*)GP(pc+PC_CONTROL_ROTATION+8); }
    if(dt>0 && (yaw || pitch)){
      u64 x0=c->x[0]; V128 v0=c->v[0];
      if(yaw) guest_call_float(c,g_add_yaw,pc,(float)yaw);
      if(pitch) guest_call_float(c,g_add_pitch,pc,(float)pitch);
      c->x[0]=x0; c->v[0]=v0;
      if(measure){ test_lock(); T.applied_yaw+=yaw; T.applied_pitch+=pitch; test_unlock(); }
    }
    orig(c,entry);
    if(measure){
      double dp=wrap180(*(double*)GP(pc+PC_CONTROL_ROTATION)-cp0), dy=wrap180(*(double*)GP(pc+PC_CONTROL_ROTATION+8)-cy0);
      test_lock();
      T.cam_pitch+=dp; T.cam_yaw+=dy;
      if(T.state==3 && plat_ns()-T.t0>300000000ull){
        T.state=4;
        double ey= T.want_yaw? 100.0*(T.cam_yaw-T.want_yaw)/T.want_yaw : 0, ep= T.want_pitch? 100.0*(T.cam_pitch-T.want_pitch)/T.want_pitch : 0;
        LOG("gyro test: yaw integral x sensitivity %.3f deg, applied %.3f, camera turned %.3f (%+.2f%%) -> %s\n",T.want_yaw,T.applied_yaw,T.cam_yaw,ey,fabs(ey)<=2? "PASS" : "FAIL");
        LOG("gyro test: pitch integral x sensitivity %.3f deg, applied %.3f, camera turned %.3f (%+.2f%%) -> %s\n",T.want_pitch,T.applied_pitch,T.cam_pitch,ep,fabs(ep)<=2? "PASS" : "FAIL");
      }
      test_unlock();
    }
    return;
  }
  orig(c,entry);
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
  /* AddPitchInput / AddYawInput start with pacibsp; str d8, [sp, #-0x30]!; UpdateRotation with pacibsp; sub sp, sp, #0x90 and the
     game's override with ldrb w8, [x0, #0x73c] */
  if(gr32(g_add_yaw)!=0xd503237fu || gr32(g_add_yaw+4)!=0xfc1d0fe8u || gr32(g_add_pitch+4)!=0xfc1d0fe8u
     || !g_disp[(g_add_yaw-g_code_lo)/4] || !g_disp[(g_add_pitch-g_code_lo)/4]){ LOG("gyro: camera functions not found, gyro unavailable\n"); return; }
  int a=bind(mod,UPDATE_ROTATION,0xd503237fu,update_rotation_base,&rot_orig);
  int b=bind(mod,UPDATE_ROTATION_SUB,0x395cf008u,update_rotation_sub,&rot_sub_orig);
  if(G.test){ test_lock(); T.state=1; test_unlock(); }
  LOG("gyro: bound to the camera (UpdateRotation %s, override %s)%s\n",a?"yes":"no",b?"yes":"no",G.test?", synthetic test armed":"");
}
