/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* win32 backend: window, WGL, keyboard, XInput.
   F1 help  F3 screenshot  F4 fps limit  F10 PC settings  F11 fullscreen  Esc quit */
#include "rt.h"
#include "audio.h"
#include "gamepad_win32.h"
#include "desktop_input.h"
#include "window_config.h"
#include <windows.h>
#include <math.h>
#include <mmsystem.h>  /* timeBeginPeriod (windows.h leaves it out with WIN32_LEAN_AND_MEAN) */

static HWND g_hwnd; static HDC g_hdc; static HGLRC g_ctx, g_wctx[3]; static volatile int g_wnext;
static volatile int g_ww=1280, g_wh=720; static u64 keys_held; static int g_fullscreen=0, g_want_fs=1;
static WINDOWPLACEMENT g_wp={sizeof(WINDOWPLACEMENT)};
static LONG g_requested_outer_w, g_requested_outer_h;

static int g_seconds=0; static char g_shot[1024]; static u64 g_start; static volatile int g_shot_req;
static char g_script[8192];
static int g_kb_player=1;   /* keyboard_player: who the keyboard and the mouse drive */
static char g_script2[8192];
/* script_file: test inputs in steps, written by a harness that watches the game (tools/visual_suite/stepper.py): the file holds a step name,
   then player 1's and player 2's scripts (the --script format), timed from the moment the step is read. Its clock leaves out the time the
   renderer spent waiting for shaders being translated (g_swr_stall_ns), so a held stick moves a character as far in a first session as later */
static char g_script_file[1024]; static u64 g_script_base_ms; static int g_script_steps;
static HANDLE g_ready;
static int g_portrait_window;
static volatile LONG g_mouse_mode, g_mouse_invert, g_mouse_dx, g_mouse_dy, g_mouse_buttons;
static volatile LONG g_mouse_sensitivity=100; /* F1 scale, curved: 10% = 5x, 500% = 1250x */
static int g_controls_open;
static LONG g_settings_open;
static int g_vibration=1;
static int settings_open(void){ return __atomic_load_n(&g_settings_open,__ATOMIC_ACQUIRE)!=0; }
static u64 g_mouse_time;
static i32 g_mouse_rx, g_mouse_ry;
static void mouse_capture(void){
  if(g_portrait_window) return;
  int active=g_mouse_mode && !g_controls_open && !settings_open() && GetForegroundWindow()==g_hwnd && !IsIconic(g_hwnd);
  if(active){ RECT r; GetClientRect(g_hwnd,&r); MapWindowPoints(g_hwnd,NULL,(POINT*)&r,2); ClipCursor(&r); }
  else ClipCursor(NULL);
  InterlockedExchange(&g_mouse_dx,0); InterlockedExchange(&g_mouse_dy,0);
  InterlockedExchange(&g_mouse_buttons,0);
}

extern int g_opt_fps;

static int g_ls_mode=0; static char g_ls_path[1024];

void backend_ini(const char* k, const char* v){
  if(!strcmp(k,"fullscreen")) g_want_fs=atoi(v);
  else if(!strcmp(k,"lossless_scaling")) g_ls_mode=atoi(v);
  else if(!strcmp(k,"lossless_path")) strncpy(g_ls_path,v,1023);
  else if(!strcmp(k,"seconds")) g_seconds=atoi(v);
  else if(!strcmp(k,"shot")) strncpy(g_shot,v,1023);
  else if(!strcmp(k,"audio_slowdown")) g_opt_audio_slowdown=atoi(v)!=0;
  else if(!strcmp(k,"mouse_right_stick")) g_mouse_mode=atoi(v)!=0;
  else if(!strcmp(k,"mouse_sensitivity")) g_mouse_sensitivity=mouse_sensitivity_percent(v);
  else if(!strcmp(k,"mouse_invert")) g_mouse_invert=atoi(v)&3;
  else if(!strcmp(k,"keyboard_player")) __atomic_store_n(&g_kb_player,atoi(v)==2?2:1,__ATOMIC_RELAXED);
  else if(!strcmp(k,"vibration")) __atomic_store_n(&g_vibration,atoi(v)!=0,__ATOMIC_RELAXED);
  else if(!strcmp(k,"script")) strncpy(g_script,v,8191);
  else if(!strcmp(k,"script2")) strncpy(g_script2,v,8191);
  else if(!strcmp(k,"script_file")) strncpy(g_script_file,v,1023);
  else if(!strcmp(k,"renderer")){   /* opengl (default), vulkan (the NVN layer, runtime/nvn) or eden (edenvk.h) */
    { static int set_nvn; int nv= !strcmp(v,"vulkan");
#ifdef SWR_HAVE_NVN
      if(nv && !getenv("SWR_GPU")){ _putenv("SWR_GPU=nvn"); if(!getenv("NXRT_GFX")) _putenv("NXRT_GFX=vulkan"); set_nvn=1; }
      else if(!nv && set_nvn){ _putenv("SWR_GPU="); set_nvn=0; }
#else
      if(nv) v="eden";   /* built without the NVN layer: Eden's renderer */
#endif
    }
    extern int g_edenvk_want; static int set_ww; g_edenvk_want= !strcmp(v,"eden");
    /* Vulkan needs to know which guest pages the CPU wrote: guest memory gets reserved with write tracking (read before
       mem_init); a later renderer line that isn't vulkan takes it back */
    if(g_edenvk_want){ if(!getenv("SWR_WW")){ _putenv("SWR_WW=1"); set_ww=1; } }
    else if(set_ww){ _putenv("SWR_WW="); set_ww=0; } }
}

/* renderer=vulkan: the window the Vulkan surface is made on (2 = Win32) */
int backend_vk_window(void** display, void** surface, int* w, int* h){ *display=NULL; *surface=(void*)g_hwnd; *w=g_ww; *h=g_wh; return g_hwnd? 2 : 0; }

int video_has_gl(void){ return 1; }

void win_fatal_box(const char* msg){
  if(!getenv("SWR_PORTRAIT_WINDOW")) MessageBoxA(g_hwnd,msg,"SwitchRecomp",MB_OK|MB_ICONERROR);
}

static void set_fullscreen(int on){
  DWORD st=GetWindowLong(g_hwnd,GWL_STYLE);
  if(on && !g_fullscreen){
    MONITORINFO mi={sizeof mi};
    if(GetWindowPlacement(g_hwnd,&g_wp) && GetMonitorInfo(MonitorFromWindow(g_hwnd,MONITOR_DEFAULTTOPRIMARY),&mi)){
      SetWindowLong(g_hwnd,GWL_STYLE,st&~WS_OVERLAPPEDWINDOW);
      SetWindowPos(g_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_NOOWNERZORDER|SWP_FRAMECHANGED); }
    ShowCursor(FALSE); g_fullscreen=1;
  } else if(!on && g_fullscreen){
    SetWindowLong(g_hwnd,GWL_STYLE,st|WS_OVERLAPPEDWINDOW); SetWindowPlacement(g_hwnd,&g_wp);
    SetWindowPos(g_hwnd,NULL,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED);
    ShowCursor(TRUE); g_fullscreen=0; }
}

/* optional: launch the lossless scaling app next to the game (lossless_scaling=1). this path doesn't touch its
   dll, the app just captures our window */
static int file_exists_(const char* p){ DWORD a=GetFileAttributesA(p); return a!=INVALID_FILE_ATTRIBUTES && !(a&FILE_ATTRIBUTE_DIRECTORY); }

static int ls_find(char* out){
  if(g_ls_path[0] && file_exists_(g_ls_path)){ strcpy(out,g_ls_path); return 1; }
  static char libs[10][600]; int nl=0;
  typedef LONG (WINAPI*RGV)(HKEY,LPCSTR,LPCSTR,DWORD,LPDWORD,PVOID,LPDWORD);
  HMODULE a=LoadLibraryA("advapi32.dll"); RGV rgv= a? (RGV)GetProcAddress(a,"RegGetValueA") : NULL;
  char sp[600]; DWORD sz=sizeof sp;
  if(rgv && rgv(HKEY_CURRENT_USER,"Software\\Valve\\Steam","SteamPath",0x0000ffff,NULL,sp,&sz)==0 && sp[0]){ strncpy(libs[nl],sp,599); libs[nl][599]=0; nl++; }
  strcpy(libs[nl++],"C:\\Program Files (x86)\\Steam"); strcpy(libs[nl++],"C:\\Program Files\\Steam");
  { char vdf[700]; snprintf(vdf,sizeof vdf,"%s\\steamapps\\libraryfolders.vdf",libs[0]); FILE* f=fopen(vdf,"r");
    if(f){ char ln[800]; while(fgets(ln,sizeof ln,f) && nl<10){ char* p=strstr(ln,"\"path\""); if(!p) continue; p=strchr(p+6,'"'); if(!p) continue; p++; char* e=strchr(p,'"'); if(!e) continue; *e=0;
        int k=0; for(char* q=p;*q && k<599;q++){ if(q[0]=='\\' && q[1]=='\\') q++; libs[nl][k++]=*q; } libs[nl][k]=0; nl++; }
      fclose(f); } }
  for(int i=0;i<nl;i++){ char c[800]; snprintf(c,sizeof c,"%s\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe",libs[i]); for(char* q=c;*q;q++) if(*q=='/') *q='\\';
    if(file_exists_(c)){ strcpy(out,c); return 1; } }
  return 0; }

static void ls_launch(void){
  if(!g_ls_mode) return;
  if(FindWindowA(NULL,"Lossless Scaling")){ LOG("lossless scaling: already running\n"); return; }
  char p[1024]; if(!ls_find(p)){ LOG("lossless scaling: not found (install it from Steam, or set lossless_path=C:\\...\\LosslessScaling.exe in the .ini)\n"); return; }
  char dir[1024]; strcpy(dir,p); char* sl=strrchr(dir,'\\'); if(sl) *sl=0;
  STARTUPINFOA si; memset(&si,0,sizeof si); si.cb=sizeof si; si.dwFlags=STARTF_USESHOWWINDOW; si.wShowWindow=SW_SHOWMINNOACTIVE; PROCESS_INFORMATION pi;
  if(CreateProcessA(p,NULL,NULL,NULL,FALSE,0,NULL,dir,&si,&pi)){ CloseHandle(pi.hThread); CloseHandle(pi.hProcess); LOG("lossless scaling: started (%s)\n",p); }
  else LOG("lossless scaling: could not open %s (error %lu)\n",p,(unsigned long)GetLastError());
  SetForegroundWindow(g_hwnd); }

/* window title update for lsfg on/off, posted from the GPU thread */
static char g_title[256];

void backend_lsfg_state(int on, int mult){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+7,(WPARAM)on,(LPARAM)mult); }

/* F7 recording state in the title bar */
volatile int g_rec_toggle;

void backend_rec_state(int on){ if(g_hwnd) PostMessageA(g_hwnd,WM_APP+8,(WPARAM)on,0); }

/* F1: controls and the mouse/audio options, drawn like the recompiler's settings page (same colors and layout: light
   gray page, rows with the value in cyan, cyan frame on the selected row, button hints at the bottom). the options
   get saved in the game's ini */
#define CW_W 760
#define CW_H 620
#define CW_ROW 40
#define CW_LIST_Y 84
enum { CW_GAME_ROWS=4, CW_SCENE_ROW=4 };
static const int cw_scene_heights[]={0,540,720,900,1080};
static int cw_rows(void){ return CW_GAME_ROWS+orbitals_scene_available(); }
static const char* inversion_names[]={"Normal", "Left / right inverted", "Up / down inverted", "Both inverted"};
static const char* const cw_keys[][2]={
  {"D-Pad","Arrows"}, {"Left stick","W A S D"}, {"Right stick","I J K L (or the mouse)"}, {"A  B  X  Y","X/Space  Z  C  V"},
  {"L  R  ZL  ZR","Q  E  1  3"}, {"+  -","Enter  Backspace"}, {"Controller","SDL3 or XInput gamepads"}};
static const char* const cw_fkeys[][2]={
  {"F1","This page"}, {"F3","Screenshot"}, {"F4","FPS limit on/off"}, {"F5","Diagnostic trace"},
  {"F7","Record video"}, {"F8","Full resolution capture"}, {"F11","Fullscreen"}, {"Esc","Quit the game"}};
static int cw_sel;
static HFONT cw_font(int px, int weight){ return CreateFontA(-px,0,0,0,weight,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI"); }
static void cw_value(int i, char* o, size_t n){
  switch(i){
    case 0: snprintf(o,n,"%s",g_mouse_mode?"Mouse":"I J K L keys"); break;
    case 1: snprintf(o,n,"%s",inversion_names[g_mouse_invert&3]); break;
    case 2: snprintf(o,n,"%s%ld%%%s",g_mouse_sensitivity>10?"<  ":"",g_mouse_sensitivity,g_mouse_sensitivity<500?"  >":""); break;
    case CW_SCENE_ROW: {
      int height=orbitals_scene_requested();
      if(height) snprintf(o,n,"<  %dp  >",height);
      else snprintf(o,n,"<  Launch setting  >");
      break; }
    default: snprintf(o,n,"%s",g_opt_audio_slowdown?"On":"Off"); } }
static void cw_change(int i, int dir){
  extern void ini_set(const char*,const char*); char value[8];
  if(i==0){ g_mouse_mode=!g_mouse_mode; snprintf(value,sizeof value,"%ld",g_mouse_mode); ini_set("mouse_right_stick",value); }
  else if(i==1){ g_mouse_invert=(g_mouse_invert+(dir<0?3:1))&3; snprintf(value,sizeof value,"%ld",g_mouse_invert); ini_set("mouse_invert",value); }
  else if(i==2){ LONG n= dir==0? 100 : g_mouse_sensitivity+dir*10; if(n<10) n=10; if(n>500) n=500; g_mouse_sensitivity=n;
    snprintf(value,sizeof value,"%.2f",n/100.0); ini_set("mouse_sensitivity",value); }
  else if(i==CW_SCENE_ROW){
    int current=orbitals_scene_requested();
    int count=sizeof cw_scene_heights/sizeof *cw_scene_heights, index=0;
    for(int k=1;k<count;k++) if(cw_scene_heights[k]==current){ index=k; break; }
    orbitals_scene_request(cw_scene_heights[(index+(dir<0?count-1:1))%count]);
  }
  else { g_opt_audio_slowdown=!g_opt_audio_slowdown; ini_set("audio_slowdown",g_opt_audio_slowdown?"1":"0"); } }
static void cw_rect(HDC dc, int x, int y, int w, int h, COLORREF c){ RECT r={x,y,x+w,y+h}; HBRUSH b=CreateSolidBrush(c); FillRect(dc,&r,b); DeleteObject(b); }
static void cw_text(HDC dc, HFONT f, const char* t, int x, int y, int w, int h, UINT fmt, COLORREF c){
  RECT r={x,y,x+w,y+h}; SelectObject(dc,f); SetTextColor(dc,c); DrawTextA(dc,t,-1,&r,fmt|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX); }
/* a round button hint like the recompiler's bottom bar: dark circle with the letter, then the action */
static int cw_hint(HDC dc, HFONT fl, HFONT ft, int right, int y, const char* key, const char* what){
  SIZE sz; SelectObject(dc,ft); GetTextExtentPoint32A(dc,what,(int)strlen(what),&sz);
  int x=right-sz.cx; cw_text(dc,ft,what,x,y-16,sz.cx+2,32,DT_LEFT,RGB(45,45,45));
  HBRUSH b=CreateSolidBrush(RGB(45,45,45)); HPEN pn=(HPEN)GetStockObject(NULL_PEN); SelectObject(dc,b); SelectObject(dc,pn);
  int cx=x-24; Ellipse(dc,cx-14,y-14,cx+15,y+15); DeleteObject(b);
  cw_text(dc,fl,key,cx-14,y-15,29,29,DT_CENTER,RGB(255,255,255));
  return cx-30; }
static void cw_paint(HWND h){
  PAINTSTRUCT ps; HDC wdc=BeginPaint(h,&ps);
  HDC dc=CreateCompatibleDC(wdc); HBITMAP bm=CreateCompatibleBitmap(wdc,CW_W,CW_H); HGDIOBJ obm=SelectObject(dc,bm);
  SetBkMode(dc,TRANSPARENT);
  HFONT fh=cw_font(30,FW_NORMAL), fb=cw_font(21,FW_NORMAL), fs=cw_font(17,FW_NORMAL), fk=cw_font(16,FW_SEMIBOLD);
  const COLORREF bg=RGB(235,235,235), text=RGB(45,45,45), dim=RGB(130,130,130), accent=RGB(8,175,200), sep=RGB(205,205,205);
  cw_rect(dc,0,0,CW_W,CW_H,bg);
  cw_text(dc,fh,"Controls",40,18,500,48,DT_LEFT,text);
  cw_rect(dc,20,66,CW_W-40,1,text);
  static const char* const names[]={"Right stick","Mouse direction","Mouse sensitivity","Smooth audio slowdown","3D resolution (this session)"};
  int rows=cw_rows();
  for(int i=0;i<rows;i++){ int y=CW_LIST_Y+i*CW_ROW; char v[64]; cw_value(i,v,sizeof v);
    if(i) cw_rect(dc,40,y,CW_W-80,1,sep);
    cw_text(dc,fb,names[i],64,y,360,CW_ROW,DT_LEFT,text);
    cw_text(dc,fb,v,CW_W-380,y,316,CW_ROW,DT_RIGHT,accent); }
  { /* the selected row: cyan frame with round corners, like the recompiler's cursor */
    int y=CW_LIST_Y+cw_sel*CW_ROW; HPEN pn=CreatePen(PS_SOLID,4,accent); SelectObject(dc,pn); SelectObject(dc,GetStockObject(NULL_BRUSH));
    RoundRect(dc,38,y-1,CW_W-38,y+CW_ROW+2,10,10); DeleteObject(pn); }
  int ky=CW_LIST_Y+rows*CW_ROW+18;
  cw_text(dc,fs,"KEYBOARD",64,ky,300,24,DT_LEFT,dim); cw_text(dc,fs,"KEYS",410,ky,300,24,DT_LEFT,dim);
  for(int i=0;i<7;i++){ int y=ky+24+i*26;
    cw_text(dc,fs,cw_keys[i][0],64,y,120,28,DT_LEFT,text); cw_text(dc,fs,cw_keys[i][1],170,y,220,28,DT_LEFT,accent); }
  for(int i=0;i<8;i++){ int y=ky+24+i*26;
    cw_text(dc,fs,cw_fkeys[i][0],410,y,50,28,DT_LEFT,accent); cw_text(dc,fs,cw_fkeys[i][1],460,y,260,28,DT_LEFT,text); }
  cw_rect(dc,30,CW_H-60,CW_W-60,1,text);
  int x=cw_hint(dc,fk,fb,CW_W-40,CW_H-30,"A","Change");
  cw_hint(dc,fk,fb,x-20,CW_H-30,"B","Back");
  cw_text(dc,fs,"Esc or F1: back to the game",40,CW_H-46,300,32,DT_LEFT,dim);
  BitBlt(wdc,0,0,CW_W,CW_H,dc,0,0,SRCCOPY);
  DeleteObject(fh); DeleteObject(fb); DeleteObject(fs); DeleteObject(fk);
  SelectObject(dc,obm); DeleteObject(bm); DeleteDC(dc); EndPaint(h,&ps); }
static LRESULT CALLBACK controls_proc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: cw_paint(h); return 0;
    case WM_KEYDOWN:
      if(w==VK_ESCAPE || w==VK_F1 || w==VK_BACK || w=='Z'){ DestroyWindow(h); return 0; }
      if(w==VK_UP || w=='W'){ if(cw_sel>0) cw_sel--; }
      else if(w==VK_DOWN || w=='S'){ if(cw_sel<cw_rows()-1) cw_sel++; }
      else if(w==VK_LEFT || w=='A') cw_change(cw_sel,-1);
      else if(w==VK_RIGHT || w=='D' || w==VK_RETURN || w==VK_SPACE || w=='X') cw_change(cw_sel,+1);
      else if(w=='R' && cw_sel==2) cw_change(2,0);
      InvalidateRect(h,NULL,FALSE); return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
      int x=(short)LOWORD(l), y=(short)HIWORD(l), i=(y-CW_LIST_Y)/CW_ROW;
      if(y>=CW_LIST_Y && i>=0 && i<cw_rows() && x>=38 && x<CW_W-38){
        if(i!=cw_sel){ cw_sel=i; InvalidateRect(h,NULL,FALSE); }
        /* Arrow rows also accept clicking either side of the value. */
        if(m!=WM_MOUSEMOVE){ cw_change(i, m==WM_RBUTTONDOWN || (i==2 && x<CW_W/2) || (i==CW_SCENE_ROW && x<CW_W-200)? -1 : +1); InvalidateRect(h,NULL,FALSE); } }
      else if(m==WM_LBUTTONDOWN && y>CW_H-60) DestroyWindow(h);
      return 0; }
    case WM_CLOSE: DestroyWindow(h); return 0; }
  return DefWindowProcA(h,m,w,l); }
static void show_controls(void){
  int fs=g_fullscreen; g_controls_open=1; keys_held=0; mouse_capture(); if(fs) set_fullscreen(0);
  HINSTANCE hi=GetModuleHandle(NULL); WNDCLASSA wc={0}; wc.lpfnWndProc=controls_proc; wc.hInstance=hi;
  wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=NULL; wc.lpszClassName="SwitchRecompControls"; RegisterClassA(&wc);
  RECT parent; GetWindowRect(g_hwnd,&parent); RECT r={0,0,CW_W,CW_H}; AdjustWindowRect(&r,WS_CAPTION|WS_SYSMENU,FALSE);
  int ww=r.right-r.left, wh=r.bottom-r.top, x=(parent.left+parent.right-ww)/2, y=(parent.top+parent.bottom-wh)/2; if(x<0) x=0; if(y<0) y=0;
  HWND h=CreateWindowA(wc.lpszClassName,"SwitchRecomp - Controls",WS_CAPTION|WS_SYSMENU,x,y,ww,wh,g_hwnd,NULL,hi,NULL);
  EnableWindow(g_hwnd,FALSE); ShowWindow(h,SW_SHOW); SetFocus(h);
  MSG msg; while(IsWindow(h) && GetMessageA(&msg,NULL,0,0)>0){ TranslateMessage(&msg); DispatchMessageA(&msg); }
  EnableWindow(g_hwnd,TRUE); SetForegroundWindow(g_hwnd); if(fs) set_fullscreen(1); g_controls_open=0; keys_held=0; mouse_capture();
}
static u64 keymap(WPARAM k){
  switch(k){ case 'X': case VK_SPACE: return 1ull<<0; case 'Z': return 1ull<<1; case 'C': return 1ull<<2; case 'V': return 1ull<<3;
    case 'Q': return 1ull<<6; case 'E': return 1ull<<7; case '1': return 1ull<<8; case '3': return 1ull<<9;
    case VK_RETURN: return 1ull<<10; case VK_BACK: return 1ull<<11;
    case VK_LEFT: return 1ull<<12; case VK_UP: return 1ull<<13; case VK_RIGHT: return 1ull<<14; case VK_DOWN: return 1ull<<15;
    /* stick keys, turned into axes later */
    case 'A': return 1ull<<40; case 'W': return 1ull<<41; case 'D': return 1ull<<42; case 'S': return 1ull<<43;
    case 'J': return 1ull<<44; case 'I': return 1ull<<45; case 'L': return 1ull<<46; case 'K': return 1ull<<47; }
  return 0; }

#include "settings_win32.inc"

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l){
  switch(m){
  case WM_CLOSE: LOG("window: closed by the user or the system\n"); g_quit=1; return 0;
  case WM_QUERYENDSESSION: LOG("window: Windows is logging off / shutting down\n"); break;
  case WM_POWERBROADCAST: if(w==PBT_APMSUSPEND) LOG("window: the PC is going to sleep\n"); else if(w==PBT_APMRESUMEAUTOMATIC) LOG("window: the PC resumed from sleep\n"); break;
  case WM_MOVE: mouse_capture(); break;
  case WM_SETFOCUS: mouse_capture(); return 0;
  case WM_GETMINMAXINFO: {
    MINMAXINFO* limits=(MINMAXINFO*)l;
    /* Windows normally clamps a decorated window's outer size to the monitor. Honor a requested client size. */
    if(limits->ptMaxTrackSize.x<g_requested_outer_w) limits->ptMaxTrackSize.x=g_requested_outer_w;
    if(limits->ptMaxTrackSize.y<g_requested_outer_h) limits->ptMaxTrackSize.y=g_requested_outer_h;
    return 0;
  }
  case WM_DPICHANGED: {
    const RECT* suggested=(const RECT*)l;
    RECT r=*suggested;
    MONITORINFO mi={sizeof mi};
    if(g_fullscreen && GetMonitorInfo(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&mi)) r=mi.rcMonitor;
    SetWindowPos(h,NULL,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
    return 0;
  }
  case WM_SIZE: mouse_capture(); g_ww=LOWORD(l); g_wh=HIWORD(l); if(g_ww<1) g_ww=1; if(g_wh<1) g_wh=1; { extern void nvn_window_resized(int,int); nvn_window_resized(g_ww,g_wh); } settings_position(); return 0;
  case WM_SYSCOMMAND: if((w&0xfff0)==SETTINGS_MENU){ settings_toggle(); return 0; } break;
  case SETTINGS_MESSAGE:
    if(!w) settings_toggle();
    else if(w==SETTINGS_FROM_GAME){
      if(!settings_window){
        settings_toggle();
        if(settings_window) LOG("settings panel: opened from the game menu\n");
      }
    }
    else if(settings_window) settings_key(w);
    return 0;
  case WM_SYSKEYDOWN: if(w==VK_RETURN){ if(!(l&(1<<30))) set_fullscreen(!g_fullscreen); return 0; } if(w==VK_F4) break; /* let Alt+F4 through */
  /* fallthrough */
  case WM_KEYDOWN:
    /* a held key auto repeats (bit 30), the one shot actions below only fire on the first press or holding F7
       would start and stop the recording every frame */
    if((l&(1<<30)) && (w==VK_ESCAPE||w==VK_F1||w==VK_F2||w==VK_F3||w==VK_F4||w==VK_F5||w==VK_F7||w==VK_F8||w==VK_F10||w==VK_F11)) return 0;
    if(settings_window){ settings_key(w); return 0; }
    if(w==VK_F10){ settings_toggle(); return 0; }
    if(w==VK_ESCAPE){ g_quit=1; return 0; }
    if(w==VK_F11){ set_fullscreen(!g_fullscreen); return 0; }
    if(w==VK_F1){ show_controls(); return 0; }
    if(w==VK_F2){ g_mouse_mode=!g_mouse_mode; mouse_capture(); return 0; }
    if(w==VK_F3){ g_shot_req=1; return 0; }
    if(w==VK_F7){ g_rec_toggle=1; return 0; }
    if(w==VK_F8){ extern volatile int g_shot8_req; g_shot8_req=1; return 0; }
    if(w==VK_F5){ extern void gpu_gl_dump_rts(void); extern void kernel_dump_threads(void); extern __thread FILE* g_log_redirect;
      CreateDirectoryA("captures",NULL); FILE* f=fopen("captures/threads.txt","w");   /* written next to the trace */
      if(f){ g_log_redirect=f; kernel_dump_threads(); g_log_redirect=NULL; fclose(f); }
      kernel_dump_threads(); gpu_gl_dump_rts(); return 0; }
    if(w==VK_F4){ g_opt_fps= g_opt_fps==0? 1 : 0; char t[64]; snprintf(t,sizeof t,"FPS limit: %s",g_opt_fps==0? "unlimited" : "60"); SetWindowTextA(h,t); { extern void ini_set(const char*, const char*); char v[8]; snprintf(v,sizeof v,"%d",g_opt_fps); ini_set("fps_limit",v); } return 0; }
    keys_held|=keymap(w); return 0;
  case WM_SYSKEYUP: case WM_KEYUP: keys_held&=~keymap(w); return 0;
  case WM_KILLFOCUS: keys_held=0; if(!g_portrait_window) ClipCursor(NULL); InterlockedExchange(&g_mouse_dx,0); InterlockedExchange(&g_mouse_dy,0); InterlockedExchange(&g_mouse_buttons,0); return 0;
  case WM_INPUT: {
    RAWINPUT raw; UINT size=sizeof raw;
    if(g_mouse_mode && !g_controls_open && !settings_open() && GetForegroundWindow()==h && GetRawInputData((HRAWINPUT)l,RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))==sizeof raw && raw.header.dwType==RIM_TYPEMOUSE){
      if(!(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)){
        InterlockedExchangeAdd(&g_mouse_dx,raw.data.mouse.lLastX); InterlockedExchangeAdd(&g_mouse_dy,raw.data.mouse.lLastY);
      }
      USHORT buttons=raw.data.mouse.usButtonFlags;
      if(buttons&RI_MOUSE_LEFT_BUTTON_DOWN) InterlockedOr(&g_mouse_buttons,1<<9);
      if(buttons&RI_MOUSE_RIGHT_BUTTON_DOWN) InterlockedOr(&g_mouse_buttons,1<<8);
      if(buttons&RI_MOUSE_LEFT_BUTTON_UP) InterlockedAnd(&g_mouse_buttons,~(1<<9));
      if(buttons&RI_MOUSE_RIGHT_BUTTON_UP) InterlockedAnd(&g_mouse_buttons,~(1<<8));
    }break;
  }
  case WM_APP+8: { char t[300]; if(w) snprintf(t,sizeof t,"%s  |  RECORDING (F7 to stop)",g_title); else snprintf(t,sizeof t,"%s  |  Video saved in the recordings folder",g_title); SetWindowTextA(h,t); return 0; }
  case WM_APP+7: { char t[160]; if(w) snprintf(t,sizeof t,"%s  |  LSFG ON x%d (frame generation)",g_title,(int)l); else snprintf(t,sizeof t,"%s  |  LSFG standby",g_title); SetWindowTextA(h,t); return 0; }
  case WM_SETCURSOR: if(!settings_window && (g_fullscreen || (g_mouse_mode && !g_controls_open && GetForegroundWindow()==h)) && LOWORD(l)==HTCLIENT){ SetCursor(NULL); return TRUE; } break;
  }
  return DefWindowProcA(h,m,w,l);
}

static char g_title[256];

static void adjust_client_rect(RECT* r, DWORD style){
  HMODULE user=GetModuleHandleA("user32.dll");
  typedef BOOL (WINAPI *AdjustForDpi)(LPRECT,DWORD,BOOL,DWORD,UINT);
  typedef UINT (WINAPI *SystemDpi)(void);
  AdjustForDpi adjust=(AdjustForDpi)GetProcAddress(user,"AdjustWindowRectExForDpi");
  SystemDpi dpi=(SystemDpi)GetProcAddress(user,"GetDpiForSystem");
  if(!adjust || !dpi || !adjust(r,style,FALSE,0,dpi())) AdjustWindowRectEx(r,style,FALSE,0);
}

static BOOL CALLBACK find_portrait_monitor(HMONITOR monitor, HDC dc, LPRECT bounds, LPARAM arg){
  (void)dc; (void)bounds;
  MONITORINFO info={0}; info.cbSize=sizeof info;
  if(!GetMonitorInfoA(monitor,&info) || (info.dwFlags&MONITORINFOF_PRIMARY)) return TRUE;
  RECT r=info.rcMonitor;
  if(r.bottom-r.top<=r.right-r.left) return TRUE;
  *(RECT*)arg=info.rcWork;
  return FALSE;
}

/* window + message pump on their own thread so nothing stalls them */
static void win_thread(void* arg){ (void)arg;
  HINSTANCE hi=GetModuleHandle(NULL);
  WNDCLASSA wc={0}; wc.style=CS_OWNDC; wc.lpfnWndProc=wndproc; wc.hInstance=hi; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
  wc.hIcon=LoadIcon(hi,MAKEINTRESOURCE(1)); if(!wc.hIcon) wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);
  wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); wc.lpszClassName="SwitchRecompWnd";
  RegisterClassA(&wc);
  WNDCLASSA panel={0}; panel.hInstance=hi; panel.lpfnWndProc=settings_proc;
  panel.hCursor=wc.hCursor; panel.lpszClassName="OrbitalsPCSettings";
  RegisterClassA(&panel);
  DWORD style=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;
  int x=CW_USEDEFAULT, y=CW_USEDEFAULT;
  int client_width=1280, client_height=720;
  int custom_size=startup_window_size(&client_width,&client_height);
  if(settings_saved_size){
    client_width=settings_saved_size*16/9; client_height=settings_saved_size; custom_size=1;
  }
  RECT work={0}, r={0,0,client_width,client_height};
  if(g_portrait_window){
    EnumDisplayMonitors(NULL,NULL,find_portrait_monitor,(LPARAM)&work);
    int available=work.right-work.left;
    if(!custom_size && available>64 && available<1328){ r.right=available-48; r.bottom=r.right*9/16; }
  } else style|=WS_VISIBLE;
  adjust_client_rect(&r,style);
  int width=r.right-r.left, height=r.bottom-r.top;
  if(custom_size){ g_requested_outer_w=width; g_requested_outer_h=height; }
  if(work.right>work.left){
    x=work.left+(work.right-work.left-width)/2;
    y=work.top+(work.bottom-work.top-height)/2;
  }
  g_hwnd=CreateWindowExA(0,"SwitchRecompWnd",g_title,style,x,y,width,height,NULL,NULL,hi,NULL);
  if(!g_hwnd){ SetEvent(g_ready); return; }
  HMENU system=GetSystemMenu(g_hwnd,FALSE);
  if(system) InsertMenuA(system,SC_CLOSE,MF_BYCOMMAND|MF_STRING,SETTINGS_MENU,"PC settings\tF10");
  RAWINPUTDEVICE rid={1,2,0,g_hwnd}; RegisterRawInputDevices(&rid,1,sizeof rid);
  g_hdc=GetDC(g_hwnd);
  PIXELFORMATDESCRIPTOR pfd={sizeof pfd,1,PFD_DRAW_TO_WINDOW|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,PFD_TYPE_RGBA,32};
  pfd.cDepthBits=24; pfd.cStencilBits=8; pfd.iLayerType=PFD_MAIN_PLANE;
  SetPixelFormat(g_hdc,ChoosePixelFormat(g_hdc,&pfd),&pfd);
  if(g_portrait_window){
    ShowWindow(g_hwnd,SW_SHOWNOACTIVATE);
    int visible=IsWindowVisible(g_hwnd)!=0, foreground=GetForegroundWindow()==g_hwnd;
    if(work.right>work.left){
      LOG("window: portrait monitor at %d,%d (%dx%d), visible=%d foreground=%d\n",x,y,width,height,visible,foreground);
    } else LOG("window: using default monitor, visible=%d foreground=%d\n",visible,foreground);
  } else if(g_want_fs) set_fullscreen(1);
  if(!g_portrait_window) ls_launch();
  mouse_capture();
  SetEvent(g_ready);
  MSG msg; while(GetMessageA(&msg,NULL,0,0)>0){ TranslateMessage(&msg); DispatchMessageA(&msg); }
  g_quit=1;
}

void video_init(const char* title){
  settings_load();
  snprintf(g_title,sizeof g_title,"%s  (F10 settings / F1 help)",title);   /* the SwitchRecomp credit stays on the startup screen and in README.md (NOTICE.txt) */
  g_portrait_window=getenv("SWR_PORTRAIT_WINDOW")!=NULL;
  /* The configured client size and the Vulkan surface use physical pixels. */
  typedef BOOL (WINAPI *SetDpiContext)(DPI_AWARENESS_CONTEXT);
  SetDpiContext dpi=(SetDpiContext)GetProcAddress(GetModuleHandleA("user32.dll"),"SetProcessDpiAwarenessContext");
  if(!dpi || !dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
  win_gamepads_init(); timeBeginPeriod(1);
  LOG("gamepads: %s backend\n",win_gamepads_backend());
  g_ready=CreateEventA(NULL,TRUE,FALSE,NULL);
  _beginthread(win_thread,1<<20,NULL);
  WaitForSingleObject(g_ready,INFINITE);
  if(!g_hwnd) fatal("Could not create the game window.");
  g_start=g_mouse_time=plat_ns(); if(getenv("SWR_REC")) g_rec_toggle=1;
  { /* under proton the driver is mesa, glthread has to be turned on from the steam launch options */
    HMODULE nt=GetModuleHandleA("ntdll.dll"); if(nt && GetProcAddress(nt,"wine_get_version"))
      LOG("Proton/Wine detected: for more FPS put   mesa_glthread=true %%command%%   in the game's Steam launch options (moves the OpenGL driver work to another core)\n"); }
  { extern int g_gpu_nvn; extern void nvn_window_set(void*); if(g_gpu_nvn){ nvn_window_set(g_hwnd); return; } }   /* renderer=vulkan: the NVN layer draws on this window, no OpenGL */
  extern void gpu_gl_start(void); gpu_gl_start();
}

/* ---------------------------------------------------------------- GL, GPU thread */
typedef HGLRC (WINAPI *CCA)(HDC,HGLRC,const int*);
typedef BOOL (WINAPI *SWI)(int);

int backend_gl_create(void){
  HGLRC tmp=wglCreateContext(g_hdc); if(!tmp) return 0;
  wglMakeCurrent(g_hdc,tmp);
  CCA cca=(CCA)wglGetProcAddress("wglCreateContextAttribsARB");
  static const int vers[4][2]={{4,6},{4,5},{4,4},{4,3}};
  for(int i=0;i<4 && !g_ctx && cca;i++){ int ca[]={0x2091,vers[i][0],0x2092,vers[i][1],0x9126,0x00000001,0}; g_ctx=cca(g_hdc,NULL,ca);
    if(g_ctx) for(int k=0;k<3;k++) g_wctx[k]=cca(g_hdc,g_ctx,ca); }   /* shared contexts for the shader compile threads */
  wglMakeCurrent(NULL,NULL); wglDeleteContext(tmp);
  if(!g_ctx){ fatal("Your graphics card does not support OpenGL 4.3.\nPlease update your GPU driver."); return 0; }
  wglMakeCurrent(g_hdc,g_ctx);
  SWI si=(SWI)wglGetProcAddress("wglSwapIntervalEXT"); extern int g_opt_vsync; if(si) si(g_opt_vsync?1:0);
  return 1; }

void* backend_gl_proc(const char* n){
  void* p=(void*)wglGetProcAddress(n);
  if(!p || p==(void*)1 || p==(void*)2 || p==(void*)3 || p==(void*)-1){ static HMODULE gl; if(!gl) gl=LoadLibraryA("opengl32.dll"); p=(void*)GetProcAddress(gl,n); }
  return p; }

void backend_gl_swap(void){ SwapBuffers(g_hdc); }

/* each compile thread takes the next shared context */
int backend_gl_worker_current(void){ int k=__atomic_fetch_add(&g_wnext,1,__ATOMIC_ACQ_REL); return k<3 && g_wctx[k] && wglMakeCurrent(g_hdc,g_wctx[k]); }

void backend_window_size(int* w, int* h){ *w=g_ww; *h=g_wh; }

void video_present_rgba(const u32* px, int w, int h, int stride){ extern void gpu_gl_present_cpu(const u32*,int,int,int); gpu_gl_present_cpu(px,w,h,stride); }

static i32 axis(u64 b, int neg, int pos){ return ((b>>pos)&1? 32767:0) - ((b>>neg)&1? 32767:0); }

static u64 scripted_buttons(const char* script, u64 elapsed){
  u64 buttons=0;
  while(*script){
    char* next;
    u64 at=strtoull(script,&next,10);
    if(*next!=':') break;
    u64 mask=strtoull(next+1,&next,16);
    u64 duration=mask>>40 ? 1400 : 300;
    if(*next=='/') duration=strtoull(next+1,&next,10);
    if(elapsed>=at && elapsed-at<duration) buttons|=mask;
    if(*next!=',') break;
    script=next+1;
  }
  return buttons;
}

/* Recorded play (SWR_INPUT_RECORD=<file>): each change of either player's final pad state (buttons and the four axes) as
   "ms player buttons lx ly rx ry", ms on the script clock from the latest script file step (from the start without a script file).
   Replay: a script file step whose player 1 line is "@<file>" plays such a file from that step, analog axes included, in place of
   the pads; the step after it ends the replay. Lets a route played once by hand be played again by the stepper. */
typedef struct { u64 t; int p; u64 b; i32 lx, ly, rx, ry; } InputRec;
static InputRec* g_replay; static int g_nreplay;
static FILE* g_input_rec;
static void replay_load(const char* path){
  free(g_replay); g_replay=NULL; g_nreplay=0;
  FILE* f=fopen(path,"rb"); if(!f){ LOG("input replay: cannot open %s\n",path); return; }
  int cap=0; char line[256];
  while(fgets(line,sizeof line,f)){
    InputRec r; unsigned long long t,b; int p;
    if(sscanf(line,"%llu %d %llx %d %d %d %d",&t,&p,&b,&r.lx,&r.ly,&r.rx,&r.ry)!=7 || p<1 || p>2) continue;
    if(g_nreplay==cap){ cap=cap?cap*2:1024; InputRec* n=realloc(g_replay,sizeof *n*(size_t)cap); if(!n) break; g_replay=n; }
    r.t=t; r.p=p; r.b=b; g_replay[g_nreplay++]=r;
  }
  fclose(f);
  LOG("input replay: %d events from %s, %.1f s\n",g_nreplay,path,g_nreplay? g_replay[g_nreplay-1].t/1000.0 : 0.0);
}
/* The script clock. By default real time without the shader translation waits. SWR_INPUT_CLOCK=game: it moves only as the game presents,
   each present counting at most 100 ms (a frame that froze on a pipeline build or anything else counts as one slow frame, about what the
   game itself advanced): a route recorded by hand (SWR_INPUT_RECORD) through freezes then replays at the same game moments, where on the
   real-time clock every freeze shifted the rest of the route. Use the same clock to record and to replay */
extern volatile long g_swr_presents;   /* hle_nvn.c, every platform */
static u64 script_clock_ms(void){
  extern volatile unsigned long long g_swr_stall_ns;
  static int game=-1; if(game<0){ const char* e=getenv("SWR_INPUT_CLOCK"); game=e && !strcmp(e,"game"); }
  u64 now=plat_ns();
  if(game){
    static long seen=-1; static u64 last, clock; enum { CAP=100000000ull };
    long n=__atomic_load_n(&g_swr_presents,__ATOMIC_RELAXED)+g_frames;
    if(seen<0){ seen=n; last=now; }
    if(n!=seen){ u64 d=now-last; clock+=d<CAP? d : CAP; last=now; seen=n; }
    u64 d=now-last; return (clock+(d<CAP? d : CAP))/1000000ull;
  }
  u64 ms=(now-g_start)/1000000ull, st=g_swr_stall_ns/1000000ull; return ms>st? ms-st : 0; }
static void script_file_poll(void){
  static u64 next; static FILETIME last; u64 now=plat_ns();
  if(now<next) return; next=now+100000000ull;
  WIN32_FILE_ATTRIBUTE_DATA a; if(!GetFileAttributesExA(g_script_file,GetFileExInfoStandard,&a)) return;
  if(!CompareFileTime(&a.ftLastWriteTime,&last)) return;
  FILE* f=fopen(g_script_file,"rb"); if(!f) return;
  static char buf[16384]; size_t n=fread(buf,1,sizeof buf-1,f); fclose(f); buf[n]=0;
  char* l[3]={buf,"",""}; int k=0;
  for(char* p=buf;*p && k<2;p++) if(*p=='\n'){ *p=0; l[++k]=p+1; }
  for(int i=0;i<3;i++){ size_t m=strlen(l[i]); while(m && (l[i][m-1]=='\r' || l[i][m-1]=='\n' || l[i][m-1]==' ')) l[i][--m]=0; }
  if(k<2) return;   /* written half: read again at the next poll */
  last=a.ftLastWriteTime;
  if(l[1][0]=='@'){ replay_load(l[1]+1); l[1]=""; l[2]=""; } else if(g_replay){ free(g_replay); g_replay=NULL; g_nreplay=0; }
  snprintf(g_script,sizeof g_script,"%s",l[1]); snprintf(g_script2,sizeof g_script2,"%s",l[2]);
  g_script_base_ms=script_clock_ms(); g_script_steps++;
  LOG("input script file: step %d '%s' from %llu ms (%.1f s of shader waits left out so far)\n",g_script_steps,l[0],
      (unsigned long long)g_script_base_ms,((plat_ns()-g_start)/1000000ull-g_script_base_ms)/1000.0);
}

static int gyro_input_allowed;
static void gyro_poll_devices(const WinGamepad* devices, int allowed){
  int want=orbitals_gyro_wanted(); win_gamepads_motion_enable(want);
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    PadMotion m[256]; int n=want? win_gamepads_motion(i,m,256) : 0;
    orbitals_gyro_pad(i,devices[i].id,devices[i].vendor,devices[i].product,devices[i].sdl_buttons,
                      devices[i].zl,m,n,devices[i].has_gyro==1,allowed);
  }
}
/* Same frontend thread as video_poll: SDL handles and its event queue have a single owner.
   Do not publish HID, advance scripts or drive settings here. Recheck focus before every batch. */
void video_motion_poll(void){
  WinGamepad devices[WIN_GAMEPAD_SLOTS];
  win_gamepads_poll(devices);
  gyro_poll_devices(devices,gyro_input_allowed && GetForegroundWindow()==g_hwnd && !g_controls_open && !settings_open());
}

void video_poll(void){
  if(g_script_file[0]) script_file_poll();
  int foreground=GetForegroundWindow()==g_hwnd && !g_controls_open;
  int focused=foreground && !settings_open();
  u64 k=focused?keys_held:0, b=k&0xFFFFull;
  i32 lx=axis(k,40,42), ly=axis(k,43,41), rx=axis(k,44,46), ry=axis(k,47,45);
  if(g_mouse_mode){
    u64 now=plat_ns(); u64 elapsed=now-g_mouse_time;
    if(!focused){ g_mouse_rx=g_mouse_ry=0; g_mouse_time=now; }
    else if(elapsed>=8000000ull){
      LONG dx=InterlockedExchange(&g_mouse_dx,0),dy=InterlockedExchange(&g_mouse_dy,0);
      mouse_stick(dx,dy,elapsed,g_mouse_sensitivity,g_mouse_invert,&g_mouse_rx,&g_mouse_ry);
      g_mouse_time=now;
    }
    if(g_mouse_rx || g_mouse_ry){ rx=g_mouse_rx; ry=g_mouse_ry; }
    if(focused) b|=(u32)InterlockedCompareExchange(&g_mouse_buttons,0,0);
  }
  WinGamepad devices[WIN_GAMEPAD_SLOTS];
  PadState pads[WIN_GAMEPAD_SLOTS]={{0}};
  win_gamepads_poll(devices);
  u64 scripted[WIN_GAMEPAD_SLOTS]={0}, elapsed=0;
  int have_script=g_script[0] || g_script2[0];
  if(have_script){
    elapsed=g_script_file[0]? script_clock_ms()-g_script_base_ms : (plat_ns()-g_start)/1000000ull;
    scripted[0]=scripted_buttons(g_script,elapsed);
    scripted[1]=scripted_buttons(g_script2,elapsed);
  }
  /* Observe held test buttons even when the panel is closed. Opening with a held button must not act on it again. */
  int panel_input=settings_controller(devices,scripted,foreground);
  gyro_input_allowed=focused && !panel_input;
  gyro_poll_devices(devices,gyro_input_allowed);
  focused=focused && !panel_input;
  static u32 last_ids[WIN_GAMEPAD_SLOTS];
  for(int i=0;i<WIN_GAMEPAD_SLOTS;i++){
    hid_rumble_update(i,devices[i].id,focused && !g_quit && __atomic_load_n(&g_vibration,__ATOMIC_RELAXED));
    if(last_ids[i]!=devices[i].id){
      LOG("gamepad: P%d %s, id=%u VID:PID=%04x:%04x\n",i+1,devices[i].id?devices[i].name:"disconnected",
          devices[i].id,devices[i].vendor,devices[i].product);
      last_ids[i]=devices[i].id;
    }
    if(focused) pads[i]=devices[i].pad;
  }
  /* keyboard_player=2: the keyboard (and the mouse as right stick) is player 2, the first gamepad alone is player 1,
     so one pad and the keyboard make two players; player 2 is connected even without a second gamepad */
  int kb2=__atomic_load_n(&g_kb_player,__ATOMIC_RELAXED)==2 && (devices[0].id || g_script[0] || g_script_file[0]);
  u64 kb_b=b; i32 kb_lx=lx, kb_ly=ly, kb_rx=rx, kb_ry=ry;
  if(kb2){ b=0; lx=ly=rx=ry=0; }
  /* Explicit test scripts provide controllers even when no physical pads are attached. */
  __atomic_store_n(&g_pad1_connected,devices[0].id!=0 || g_script[0]!=0 || g_script_file[0]!=0,__ATOMIC_RELEASE);
  __atomic_store_n(&g_pad2_connected,devices[1].id!=0 || kb2 || g_script2[0]!=0 || g_script_file[0]!=0,__ATOMIC_RELEASE);
  if(panel_input){ g_pad=(PadState){0}; g_pad2=(PadState){0}; return; }
  b|=pads[0].buttons;
  if(!lx&&!ly){ lx=pads[0].lx; ly=pads[0].ly; } if(!rx&&!ry){ rx=pads[0].rx; ry=pads[0].ry; }
  g_pad2=pads[1];
  if(kb2){ g_pad2.buttons|=kb_b;
    if(kb_lx||kb_ly){ g_pad2.lx=kb_lx; g_pad2.ly=kb_ly; } if(kb_rx||kb_ry){ g_pad2.rx=kb_rx; g_pad2.ry=kb_ry; }
    if(g_pad2.lx<-16000) g_pad2.buttons|=1ull<<16; if(g_pad2.ly>16000) g_pad2.buttons|=1ull<<17; if(g_pad2.lx>16000) g_pad2.buttons|=1ull<<18; if(g_pad2.ly<-16000) g_pad2.buttons|=1ull<<19;
    if(g_pad2.rx<-16000) g_pad2.buttons|=1ull<<20; if(g_pad2.ry>16000) g_pad2.buttons|=1ull<<21; if(g_pad2.rx>16000) g_pad2.buttons|=1ull<<22; if(g_pad2.ry<-16000) g_pad2.buttons|=1ull<<23; }
  /* stick-as-button bits 16..23 */
  if(lx<-16000) b|=1ull<<16; if(ly>16000) b|=1ull<<17; if(lx>16000) b|=1ull<<18; if(ly<-16000) b|=1ull<<19;
  if(rx<-16000) b|=1ull<<20; if(ry>16000) b|=1ull<<21; if(rx>16000) b|=1ull<<22; if(ry<-16000) b|=1ull<<23;
  if(have_script){
    u64 a=scripted[0], z=scripted[1];
    /* stick bits as the keyboard uses them: 40/42 left stick x-/x+, 43/41 y-/y+, 44/46 right stick x-/x+, 47/45 y-/y+ */
    b|=a&0xffffff;
    if(a&(5ull<<40)) lx=axis(a,40,42);
    if(a&(10ull<<40)) ly=axis(a,43,41);
    if(a&(5ull<<44)) rx=axis(a,44,46);
    if(a&(10ull<<44)) ry=axis(a,47,45);
    g_pad2.buttons|=z&0xffffff;
    if(z&(5ull<<40)) g_pad2.lx=axis(z,40,42);
    if(z&(10ull<<40)) g_pad2.ly=axis(z,43,41);
    if(z&(5ull<<44)) g_pad2.rx=axis(z,44,46);
    if(z&(10ull<<44)) g_pad2.ry=axis(z,47,45);
    static u64 last_a,last_z;
    if(a!=last_a || z!=last_z){
      LOG("input test: %llu ms P1 %llx x%d P2 %llx x%d\n",elapsed,a,lx,z,g_pad2.lx);
      last_a=a; last_z=z;
    }
  }
  g_pad.buttons=b; g_pad.lx=lx; g_pad.ly=ly; g_pad.rx=rx; g_pad.ry=ry;
  u64 since=g_script_file[0]? script_clock_ms()-g_script_base_ms : (plat_ns()-g_start)/1000000ull;
  if(g_replay){   /* the latest event of each player at or before now */
    static int at; static u64 last_base; if(last_base!=g_script_base_ms){ last_base=g_script_base_ms; at=0; }
    static InputRec cur[2];
    if(!at){ memset(cur,0,sizeof cur); }
    while(at<g_nreplay && g_replay[at].t<=since){ cur[g_replay[at].p-1]=g_replay[at]; at++; }
    g_pad.buttons=cur[0].b; g_pad.lx=cur[0].lx; g_pad.ly=cur[0].ly; g_pad.rx=cur[0].rx; g_pad.ry=cur[0].ry;
    g_pad2.buttons=cur[1].b; g_pad2.lx=cur[1].lx; g_pad2.ly=cur[1].ly; g_pad2.rx=cur[1].rx; g_pad2.ry=cur[1].ry;
  }
  static int rec_init; if(!rec_init){ rec_init=1; const char* e=getenv("SWR_INPUT_RECORD"); if(e && *e){ g_input_rec=fopen(e,"ab"); LOG("input record: %s %s\n",e,g_input_rec?"open":"cannot be opened"); } }
  if(g_input_rec){
    static PadState last[2]; static u64 last_base=~0ull; const PadState* now[2]={&g_pad,&g_pad2};
    if(last_base!=g_script_base_ms){ last_base=g_script_base_ms; fprintf(g_input_rec,"# step %d at %llu ms\n",g_script_steps,(unsigned long long)g_script_base_ms); memset(last,0,sizeof last); }
    for(int i=0;i<2;i++) if(memcmp(&last[i],now[i],sizeof last[i])){
      last[i]=*now[i]; fprintf(g_input_rec,"%llu %d %llx %d %d %d %d\n",(unsigned long long)since,i+1,(unsigned long long)now[i]->buttons,now[i]->lx,now[i]->ly,now[i]->rx,now[i]->ry); fflush(g_input_rec); }
  }
}

int backend_rumble(int player, u16 low, u16 high, u32 duration_ms){
  return win_gamepads_rumble(player,low,high,duration_ms);
}

void backend_list_gamepads(void){
  win_gamepads_init();
  win_gamepads_inventory();
}

void frontend_frame(void){
  extern u64 gpu_gl_screenshot(const char* path);
  if(g_shot_req){ g_shot_req=0; char p[MAX_PATH]; SYSTEMTIME t; GetLocalTime(&t); CreateDirectoryA("captures",NULL);
    snprintf(p,sizeof p,"captures/screenshot_%04d%02d%02d_%02d%02d%02d",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond); gpu_gl_screenshot(p); }
  if(g_seconds && plat_ns()-g_start>=(u64)g_seconds*1000000000ull){
    u64 h=gpu_gl_screenshot(g_shot[0]? g_shot : NULL); fprintf(stderr,"\nframes=%d hash=%016llx\n",g_frames,(unsigned long long)h);
    hid_rumble_stop();
    extern void save_hints(void); save_hints(); { extern void gpu_gl_rec_finish(void); gpu_gl_rec_finish(); } fflush(NULL); ExitProcess(0); }
}

int backend_glq_ok(void){ return 1; }   /* GL calls go through glq */
