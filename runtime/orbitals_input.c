/* Orbitals PC Recomp, based on SwitchRecomp by MZ Labs. AGPL-3.0, see LICENSE.txt and NOTICE.txt. */
/* Orbitals input extras: the button layout (Nintendo / Xbox / PlayStation) and gyro aiming.
   Everything here is off by default (button_layout=nintendo, gyro=off): the game then runs as before. */
#include "rt.h"
#include "orbitals_input.h"
#include <math.h>

void ini_set(const char* k, const char* v);   /* main.c: stores a setting and rewrites the ini */

/* ===================================================================== button layout
   The Switch 2 build redirects six Enhanced Input mapping contexts to Switch versions (BP_Switch2InputPlatformData, loaded from
   [EnhancedInputPlatformSettings_Switch2] in the game's Switch2Input.ini): IMC_UI_Default -> IMC_UI_PlatformSwitch moves UI accept to
   Gamepad_FaceButton_Right and back to Gamepad_FaceButton_Bottom (the Nintendo convention). The PC version of the game uses the
   unredirected contexts (accept on the bottom button). Its button icons come from DA_PlatformInputIcons, per key and per
   EPlatformType (Desktop / Playstation / Switch / Xbox), and CommonInput's controller data for Windows lists BP_InputData_Xbox (gamepad
   name XSX) and BP_InputData_Playstation (PS5).
   button_layout=xbox / playstation writes Unreal's user config layer for the Switch2 platform (Orbitals/Saved/Config/Switch2/, which
   the game reads from its RomFS) into <exe folder>/button_layout/<layout>/ and lays it over the RomFS: the redirect list is cleared
   (accept on the bottom button in menus and in game, for both players) and CommonInput gets that controller's data as its default.
   nintendo (default) adds nothing. */
static int g_layout;   /* 0 nintendo, 1 xbox, 2 playstation */

static int write_text(const char* path, const char* text){
  FILE* f=fopen(path,"wb"); if(!f) return 0;
  fputs(text,f); fclose(f); return 1;
}

const char* orbitals_layout_overlay(void){
  if(!g_layout) return NULL;
  static char root[1200];
  const char* name= g_layout==1? "xbox" : "playstation";
  char dir[1400];
  snprintf(root,sizeof root,"%sbutton_layout",g_exedir); plat_mkdir(root);
  snprintf(root,sizeof root,"%sbutton_layout/%s",g_exedir,name); plat_mkdir(root);
  static const char* parts[]={"Orbitals","Saved","Config","Switch2"};
  size_t n=(size_t)snprintf(dir,sizeof dir,"%s",root);
  for(unsigned i=0;i<4 && n<sizeof dir;i++){ n+=(size_t)snprintf(dir+n,sizeof dir-n,"/%s",parts[i]); plat_mkdir(dir); }
  char path[1500];
  snprintf(path,sizeof path,"%s/Input.ini",dir);
  int ok=write_text(path,
    "; written by the runtime (button_layout): the PC version's input mappings, accept on the bottom button\n"
    "[EnhancedInputPlatformSettings_Switch2 EnhancedInputPlatformSettings]\n"
    "!InputData=ClearArray\n");
  char game[1024];
  snprintf(game,sizeof game,
    "; written by the runtime (button_layout): %s controller data and icons\n"
    "[CommonInputPlatformSettings_Switch2 CommonInputPlatformSettings]\n"
    "DefaultGamepadName=%s\n"
    "bCanChangeGamepadType=False\n"
    "!ControllerData=ClearArray\n"
    "+ControllerData=/Game/Orbitals/Blueprints/UI/CommonUI/InputData/%s.%s_C\n",
    g_layout==1? "Xbox" : "PlayStation", g_layout==1? "XSX" : "PS5",
    g_layout==1? "BP_InputData_Xbox" : "BP_InputData_Playstation", g_layout==1? "BP_InputData_Xbox" : "BP_InputData_Playstation");
  snprintf(path,sizeof path,"%s/Game.ini",dir);
  ok&=write_text(path,game);
  if(!ok){ LOG("Orbitals: button layout %s: could not write %s\n",name,dir); return NULL; }
  LOG("Orbitals: button layout %s (Unreal user config from %s)\n",name,root);
  return root;
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
} G={0,1.5f,1.5f,20,0,3.0f,0,0,0};

void orbitals_input_ini(const char* k, const char* v){
  if(!strcmp(k,"button_layout")){ g_layout= !strcmp(v,"xbox")? 1 : !strcmp(v,"playstation")? 2 : 0; }
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
}
int orbitals_gyro_wanted(void){ return G.mode || G.test || G.calibrate; }

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
} GyroPad;
static GyroPad P[SLOTS];

/* the turn waiting for the player's next UpdateRotation, degrees; written by the frontend thread, taken by the game thread */
static struct { int lock; double yaw, pitch; u64 taken_ns; } Acc[SLOTS];
static void acc_lock(int s){ while(__atomic_exchange_n(&Acc[s].lock,1,__ATOMIC_ACQUIRE)) {} }
static void acc_unlock(int s){ __atomic_store_n(&Acc[s].lock,0,__ATOMIC_RELEASE); }
static void acc_clear(int s){ acc_lock(s); Acc[s].yaw=Acc[s].pitch=0; acc_unlock(s); }

/* stored calibration: lines "vid:pid bx by bz" (rad/s) in gyro_calibration.txt beside the exe */
static void calib_path(char* p, size_t n){ snprintf(p,n,"%sgyro_calibration.txt",g_exedir); }
static int calib_load(GyroPad* g){
  char p[1200]; calib_path(p,sizeof p); FILE* f=fopen(p,"rb"); if(!f) return 0;
  char line[256]; int found=0;
  while(fgets(line,sizeof line,f)){
    unsigned vid,pid; float b[3];
    if(sscanf(line,"%x:%x %f %f %f",&vid,&pid,&b[0],&b[1],&b[2])==5 && vid==g->vendor && pid==g->product){ memcpy(g->bias,b,sizeof b); found=1; }
  }
  fclose(f); return found;
}
static void calib_store(const GyroPad* g){
  char p[1200], tmp[1300]; calib_path(p,sizeof p); snprintf(tmp,sizeof tmp,"%s.tmp",p);
  FILE* in=fopen(p,"rb"); FILE* out=fopen(tmp,"wb"); if(!out){ if(in) fclose(in); return; }
  if(in){ char line[256]; while(fgets(line,sizeof line,in)){ unsigned vid,pid; if(sscanf(line,"%x:%x",&vid,&pid)==2 && vid==g->vendor && pid==g->product) continue; fputs(line,out); } fclose(in); }
  fprintf(out,"%04x:%04x %.6f %.6f %.6f\n",g->vendor,g->product,g->bias[0],g->bias[1],g->bias[2]);
  fclose(out); remove(p); rename(tmp,p);
}

static void still_reset(GyroPad* g, u64 t, const float* raw){
  g->still_t0=t; g->still_n=0;
  for(int k=0;k<3;k++){ g->still_sum[k]=0; g->still_min[k]=g->still_max[k]=raw[k]; }
}
/* calibration: a window where every axis stays within 0.02 rad/s of itself and under 0.04 rad/s (2.3 deg/s: a pad's offset is
   well under that, so a slow steady turn is not taken for one). The automatic one never moves a stored calibration by more than
   0.015 rad/s. Manual (gyro_calibrate=1, or the gyro button held): a 2 s / 3 s window that starts once asked */
static void still_sample(int s, GyroPad* g, u64 t, const float* raw, int manual_hold){
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
    g->calibrated=2; calib_store(g);
    LOG("gyro: P%d calibrated and stored for %04x:%04x: %.4f %.4f %.4f rad/s (%d samples)\n",s+1,g->vendor,g->product,g->bias[0],g->bias[1],g->bias[2],g->still_n);
    if(G.calibrate){ G.calibrate=0; ini_set("gyro_calibrate","0"); }
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
