"""Button layout: the icon lookup wrapper reached from generated code, and the pieces in runtime/orbitals_buttons.h.

Generates a guest caller with the real codegen (Program / emit_function over real A64 words), links it with the real
icon_lookup / ei_clear from runtime/orbitals_buttons.h and a C stand-in for the game's lookup, and runs it.
Run: python3 -m unittest tests.test_button_layout (needs a host C compiler: gcc, clang or CC_HOST)."""
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

import codegen
from nxloader import Module

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ORBITALS_MAIN = '0f8ef52cf4eee37628f366a539d5d6ad'
FIXTURE_ID = bytes.fromhex('aa' * 16)
BASE = 0x8000000
LOOKUP = 0x40          # fixture offsets: caller at 0, lookup at 0x40


def fixture_program(dynamic):
    words = [0xd2800062,   # mov x2, #3 (Switch)
             0x9400000f,   # bl +0x3c -> 0x40
             0xd65f03c0]   # ret
    image = bytearray(0x80)
    for i, w in enumerate(words): struct.pack_into('<I', image, 4 * i, w)
    struct.pack_into('<I', image, LOOKUP, 0xd65f03c0)
    m = Module('main', bytes(image), (0, 0x80), (0, 0), (0, 0), 0, FIXTURE_ID)
    m.base = BASE
    saved = dict(codegen.DYNAMIC_CALLS)
    try:
        codegen.DYNAMIC_CALLS[FIXTURE_ID.hex()] = (LOOKUP,) if dynamic else ()
        prog = codegen.Program([m])
    finally:
        codegen.DYNAMIC_CALLS.clear(); codegen.DYNAMIC_CALLS.update(saved)
    caller = codegen.Func(BASE, [BASE, BASE + 4, BASE + 8], m)
    lookup = codegen.Func(BASE + LOOKUP, [BASE + LOOKUP], m)
    caller.leaders = {BASE, BASE + 8}; lookup.leaders = {BASE + LOOKUP}
    funcs = {BASE: caller, BASE + LOOKUP: lookup}
    return prog, caller, funcs


PROMPT = 0x40         # prompt fixture: caller A at 0, caller B at 0x20, both bl the action prompt at 0x40


def prompt_fixture_program(dynamic):
    image = bytearray(0x80)
    for off, w in ((0x00, 0x94000010),    # A: bl +0x40 -> 0x40
                   (0x04, 0xd65f03c0),    #    ret
                   (0x20, 0x94000008),    # B: bl +0x20 -> 0x40
                   (0x24, 0xd65f03c0),    #    ret
                   (PROMPT, 0xd65f03c0)): struct.pack_into('<I', image, off, w)
    m = Module('main', bytes(image), (0, 0x80), (0, 0), (0, 0), 0, FIXTURE_ID)
    m.base = BASE
    saved = dict(codegen.DYNAMIC_CALLS)
    try:
        codegen.DYNAMIC_CALLS[FIXTURE_ID.hex()] = (PROMPT,) if dynamic else ()
        prog = codegen.Program([m])
    finally:
        codegen.DYNAMIC_CALLS.clear(); codegen.DYNAMIC_CALLS.update(saved)
    a = codegen.Func(BASE, [BASE, BASE + 4], m); a.leaders = {BASE, BASE + 4}
    b = codegen.Func(BASE + 0x20, [BASE + 0x20, BASE + 0x24], m); b.leaders = {BASE + 0x20, BASE + 0x24}
    p = codegen.Func(BASE + PROMPT, [BASE + PROMPT], m); p.leaders = {BASE + PROMPT}
    return prog, [a, b], {BASE: a, BASE + 0x20: b, BASE + PROMPT: p}


class CodegenDynamicCallTests(unittest.TestCase):
    def test_orbitals_icon_lookup_is_listed_for_the_runtimes_build(self):
        self.assertEqual(codegen.DYNAMIC_CALLS[ORBITALS_MAIN], (0x40c5020, 0x40c5ce0))
        with open(os.path.join(ROOT, 'runtime', 'orbitals_input.c')) as f: src = f.read()
        build = re.search(r'orbitals_build\[16\] = \{([^}]*)\}', src).group(1)
        self.assertEqual(bytes(int(x, 16) for x in build.split(',')).hex(), ORBITALS_MAIN)
        self.assertIn('ICON_LOOKUP=0x40c5ce0', src)
        self.assertIn('ICON_PROMPT=0x40c5020', src)

    def test_both_action_prompt_callers_go_through_the_dispatch_table(self):
        prog, callers, funcs = prompt_fixture_program(True)
        self.assertEqual(prog.dyn_calls, {BASE + PROMPT})
        for f in callers:
            c = codegen.emit_function(prog, f, funcs)
            self.assertIn('CALLDYN(%s)' % codegen.lit(BASE + PROMPT), c)
            self.assertNotIn('F_%X(c' % (BASE + PROMPT), c)
        prog, callers, funcs = prompt_fixture_program(False)
        for f in callers: self.assertIn('F_%X(c' % (BASE + PROMPT), codegen.emit_function(prog, f, funcs))

    def test_install_signatures_match_main_bin(self):
        # The runtime's fail-closed checks (instruction words of both wrapped functions and their callers) against the real image.
        path = os.environ.get('ORBITALS_MAIN_BIN', '/tmp/claude-1000/orbdata/main.bin')
        if not os.path.exists(path): self.skipTest('main.bin not available (ORBITALS_MAIN_BIN)')
        with open(os.path.join(ROOT, 'runtime', 'orbitals_input.c')) as f: src = f.read()
        names = {'ICON_CALL_BP': 0x40b2e64, 'ICON_CALL_WIDGET': 0x4634a78, 'ICON_PROMPT_CALL_A': 0x40c4ed0, 'ICON_PROMPT_CALL_B': 0x40d3990}
        with open(path, 'rb') as f: img = f.read()
        for fn, target in (('icon_install', 0x40c5ce0), ('icon_prompt_install', 0x40c5020)):
            body = src[src.index('static void %s(' % fn):]
            table = body[body.index('sig[]={'):body.index('};')]
            pairs = re.findall(r'\{(0x[0-9a-f]+|ICON_\w+),\s*(0x[0-9a-f]+)\}', table)
            self.assertGreaterEqual(len(pairs), 8, fn)
            calls = 0
            for off, w in pairs:
                off = names[off] if off in names else int(off, 16)
                word = struct.unpack_from('<I', img, off)[0]
                self.assertEqual(word, int(w, 16), '%s %x' % (fn, off))
                if word >> 26 == 0x25:   # bl: must land on the wrapped function
                    imm = word & 0x3ffffff
                    if imm & 0x2000000: imm -= 0x4000000
                    self.assertEqual(off + imm * 4, target, '%s caller %x' % (fn, off)); calls += 1
            self.assertEqual(calls, 2, fn)

    def test_listed_target_goes_through_the_dispatch_table(self):
        prog, caller, funcs = fixture_program(True)
        self.assertEqual(prog.dyn_calls, {BASE + LOOKUP})
        c = codegen.emit_function(prog, caller, funcs)
        self.assertIn('CALLDYN(%s)' % codegen.lit(BASE + LOOKUP), c)
        self.assertNotIn('F_%X(c' % (BASE + LOOKUP), c)

    def test_unlisted_target_stays_a_direct_call(self):
        prog, caller, funcs = fixture_program(False)
        self.assertEqual(prog.dyn_calls, set())
        self.assertIn('F_%X(c' % (BASE + LOOKUP), codegen.emit_function(prog, caller, funcs))

    def test_other_build_ids_get_nothing(self):
        class M: build_id = bytes(32); base = BASE
        self.assertEqual(codegen.dynamic_call_targets([M()]), set())


PRELUDE = r'''
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
static u8 g_mem[0x10000];
GuestFn swr_interp_entry;
#define GP(a) (g_mem+(u64)(a))
static int g_retired; static int g_stale;   /* guest reads made after a callee retired the object */
static u64 g_dead_lo, g_dead_hi;            /* or of a range a callee declared dead */
#define STALE(a) do{ if(g_retired || ((a)>=g_dead_lo && (a)<g_dead_hi)) g_stale++; }while(0)
static u64 gr64(u64 a){ u64 v; STALE(a); memcpy(&v,GP(a),8); return v; }
static u32 gr32(u64 a){ u32 v; STALE(a); memcpy(&v,GP(a),4); return v; }
static void put64(u64 a,u64 v){ memcpy(GP(a),&v,8); }
static void put32(u64 a,u32 v){ memcpy(GP(a),&v,4); }
static int g_nlog, g_ntrace; static char g_trace[512];
#define LOG(...) do{ char l_[512]; snprintf(l_,sizeof l_,__VA_ARGS__); g_nlog++; \
  if(strstr(l_,"icon prompt reached")){ g_ntrace++; memcpy(g_trace,l_,sizeof l_); } }while(0)
enum { PERM_R=1, PERM_RW=3 };
static int mem_span_ok_locked(u64 a,u64 n,u32 p){ (void)p; return a+n<=sizeof g_mem; }
static int guest_span_readable(u64 a,u64 n,u64 al){ return !(a&(al-1)) && a+n<=sizeof g_mem; }
static int g_locked;
static int mem_with_range(u64 a,u64 n,void (*fn)(u64,u64,void*),void* ctx){ if(!guest_span_readable(a,n,1)) return 0;
  g_locked++; fn(a,n,ctx); g_locked--; return 1; }
static u64 g_ei_vtable;
typedef struct CPU CPU;
#include "orbitals_buttons.h"
'''

C_TEST = PRELUDE + r'''
/* the game's lookup stand-in: per platform 0 no entry, 1 entry with a NULL texture, 2 entry with a texture (0x1000 + platform) */
static int entry_kind[4]; static int calls; static int plat_seen[4];
void F_LOOKUP(CPU* RESTRICT c, u64 e){ (void)e; calls++; u8 p=(u8)c->x[2]; plat_seen[calls&3]=p;
  int k=entry_kind[p&3]; if(k) put64(c->x[3], k==2? 0x1000+p : 0); c->x[0]=k!=0; c->pc=c->x[30]; }
GENERATED


static int fails;
#define CHECK(cond,...) do{ if(!(cond)){ fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } }while(0)
/* ei_virtual through the real install (ei_wrap_vtable): vtable at VT (module offsets), code at CODE_LO. Slots 0 / 1 destructors,
   2 -> RETIRE (its callee retires the object: vtable word cleared, every later guest read counted), 3 -> PLAIN, 4 a duplicate
   of 2, 5 a word outside the code (end of the vtable). */
enum { VT=0x6000, CODE_LO=0x10000000, MOD_SIZE=0x100, OBJ=0x7000, OTHER=0x7100, MAPD=0x7400, OTHERD=0x7500, KEY=0x7800, VAL=0x7880 };
static GuestFn ei_disp[MOD_SIZE/4];
static struct { int n; u64 x[4], lr, entry; u64 val_seen; } rec[4];
static void orig_dtor(CPU* c,u64 e){ (void)c; (void)e; rec[0].n++; }
static void orig_retire(CPU* c,u64 e){ rec[2].n++; for(int i=0;i<4;i++) rec[2].x[i]=c->x[i]; rec[2].lr=c->x[30]; rec[2].entry=e;
  rec[2].val_seen=gr64(MAPD+8); put64(c->x[0],0); g_retired=1; c->pc=c->x[30]; }
static void orig_plain(CPU* c,u64 e){ rec[3].n++; for(int i=0;i<4;i++) rec[3].x[i]=c->x[i]; rec[3].entry=e; rec[3].val_seen=gr64(c->x[0]==OBJ? MAPD+8 : OTHERD+8);
  c->pc=c->x[30]; }
static void map_at(u64 o,u64 d,u64 vt){ memset(GP(o),0,0x78); put64(o,vt); put64(o+0x28,d); put32(o+0x30,1); put32(o+0x34,4);
  put32(o+0x38,1); put32(o+0x50,1); put32(o+0x54,128); put32(o+0x58,0xffffffffu); put32(o+0x5c,0);
  put64(d,KEY); put64(d+8,VAL); put64(d+16,0x0000000500000003ull); }
static void call(u64 obj,u64 slot){ static CPU c; memset(&c,0,sizeof c); c.mem=g_mem; c.x[0]=obj; c.x[1]=0x11; c.x[2]=0x22; c.x[3]=0x33;
  c.x[30]=0xabc0; u64 fn=CODE_LO+4*slot; g_retired=0; g_stale=0; ei_disp[slot](&c,fn); g_retired=0;
  CHECK(c.pc==0xabc0,"returned through the original (pc %llx)",(unsigned long long)c.pc); }
static void ei_tests(void){
  memset(g_mem,0,sizeof g_mem); memset(rec,0,sizeof rec);
  u64 words[]={0,4,8,12,8,0x7777777}; for(int i=0;i<6;i++) put64(VT+8*i,words[i]); put64(VT+48,0);
  for(int i=0;i<4;i++) ei_disp[i]= i==2? orig_retire : i==3? orig_plain : orig_dtor;
  g_ei_vtable=VT; g_neiw=0;
  int n=ei_wrap_vtable(VT,CODE_LO,MOD_SIZE,CODE_LO,CODE_LO+MOD_SIZE,ei_disp);
  CHECK(n==2 && ei_disp[0]==orig_dtor && ei_disp[1]==orig_dtor && ei_disp[2]==ei_virtual && ei_disp[3]==ei_virtual,
        "install: %d wrapped, destructors untouched, duplicate and end skipped",n);
  CHECK(ei_wrap_vtable(VT,CODE_LO,MOD_SIZE,CODE_LO,CODE_LO+MOD_SIZE,ei_disp)==2,"installing twice wraps nothing more");
  put64(KEY,0x9990); put64(VAL,0x9990);   /* two mapping contexts of one class */
  map_at(OBJ,MAPD,VT); map_at(OTHER,OTHERD,0x6800);
  u8 before[0x78], other[0x78], odata[24]; memcpy(before,GP(OBJ),0x78); memcpy(other,GP(OTHER),0x78); memcpy(odata,GP(OTHERD),24);
  /* the callee retires the object: the map is already rewritten when it starts, no guest read follows */
  call(OBJ,2);
  CHECK(rec[2].n==1 && rec[2].entry==CODE_LO+8 && rec[2].x[0]==OBJ && rec[2].x[1]==0x11 && rec[2].x[2]==0x22 && rec[2].x[3]==0x33 &&
        rec[2].lr==0xabc0,"original called once with the caller's registers (calls %d)",rec[2].n);
  CHECK(rec[2].val_seen==KEY,"redirect rewritten before the original ran (%llx)",(unsigned long long)rec[2].val_seen);
  CHECK(g_stale==0,"%d guest reads after the object was retired",g_stale);
  CHECK(!memcmp(GP(OBJ)+8,before+8,0x70) && gr64(MAPD)==KEY && gr64(MAPD+16)==0x0000000500000003ull,
        "map structure kept: count, capacity, bits, free list, key and hash");
  /* a live object: rewritten before the call, original sees it, registers unchanged */
  map_at(OBJ,MAPD,VT); call(OBJ,3);
  CHECK(rec[3].n==1 && rec[3].x[0]==OBJ && rec[3].x[1]==0x11 && rec[3].val_seen==KEY && gr64(MAPD+8)==KEY,"live object rewritten");
  /* an object of another class through the same shared virtual: called, nothing written */
  call(OTHER,3);
  CHECK(rec[3].n==2 && rec[3].x[0]==OTHER && rec[3].val_seen==VAL && !memcmp(GP(OTHER),other,0x78) && !memcmp(GP(OTHERD),odata,24),
        "unrelated object unchanged");
  /* an empty map (the call before the properties are read): nothing written; a non-object receiver (NULL, misaligned) only calls */
  map_at(OBJ,MAPD,VT); put32(OBJ+0x30,0); call(OBJ,3); CHECK(rec[3].n==3 && gr64(MAPD+8)==VAL,"empty map untouched");
  call(0,3); call(OBJ+4,3); CHECK(rec[3].n==5 && g_locked==0,"non-object receivers called through");
  /* an entry that is not in the table: nothing called */
  { static CPU c; memset(&c,0,sizeof c); c.x[0]=OBJ; int before_n=rec[3].n+rec[2].n; ei_virtual(&c,CODE_LO+0x40);
    CHECK(rec[3].n+rec[2].n==before_n,"unknown entry"); }
}
static GuestFn disp[0x20];
static u64 run(int a,int b,int c3,int want,int hooked){
  static CPU c; memset(&c,0,sizeof c); c.mem=g_mem; c.disp=disp; c.code_lo=BASE; c.code_hi=BASE+0x80;
  entry_kind[1]=a; entry_kind[2]=b; entry_kind[3]=c3; calls=0; g_icon_want=want;
  memset(disp,0,sizeof disp); disp[0]=F_CALLER; disp[LOOKUP/4]= hooked? icon_lookup : F_LOOKUP; icon_lookup_orig=F_LOOKUP;
  c.x[3]=0x800; put64(0x800,0xdead); c.x[31]=0x4000;
  F_CALLER(&c,BASE);
  CHECK(c.pc==BASE+8,"returned to the caller (pc %llx)",(unsigned long long)c.pc);
  return (c.x[0]&1)? gr64(0x800) : 0xffff;
}
int main(void){
  u64 r;
  r=run(2,2,2,1,1); CHECK(r==0x1001 && calls==1,"PlayStation texture through the generated call: %llx calls %d",(unsigned long long)r,calls);
  r=run(2,2,2,2,1); CHECK(r==0x1002 && calls==1,"Xbox texture: %llx",(unsigned long long)r);
  r=run(1,2,2,1,1); CHECK(r==0x1003 && calls==2,"NULL PlayStation texture falls back to Switch: %llx calls %d",(unsigned long long)r,calls);
  r=run(0,2,2,1,1); CHECK(r==0x1003 && calls==2,"no PlayStation entry falls back to Switch: %llx calls %d",(unsigned long long)r,calls);
  r=run(1,0,0,1,1); CHECK(r==0xffff && gr64(0x800)==0xdead && calls==2,"NULL then nothing: false, output untouched (%llx)",(unsigned long long)gr64(0x800));
  r=run(2,2,2,1,0); CHECK(r==0x1003 && calls==1,"unhooked (nintendo): Switch texture %llx",(unsigned long long)r);
  /* ei_clear: heap-backed allocation flags with one bit (NumBits 1, MaxBits 256) */
  memset(g_mem,0,sizeof g_mem);
  u64 o=0x100; put64(o+0x28,0x200); put32(o+0x30,1); put32(o+0x34,256); put64(o+0x48,0x300); put32(o+0x50,1); put32(o+0x54,256);
  put32(0x300,1); put64(0x200,0x400); put64(0x208,0x500); put64(0x400,0x999); put64(0x500,0x999);
  int ch=ei_clear(o); CHECK(ch==1 && gr64(0x208)==0x400,"heap bitmap NumBits 1: changed %d target %llx",ch,(unsigned long long)gr64(0x208));
  CHECK(gr32(o+0x30)==1 && gr64(0x200)==0x400,"structure kept");
  /* inline flags (no heap pointer) */
  put64(0x208,0x500); put64(o+0x48,0); put32(o+0x38,1); put32(0x300,0);
  ch=ei_clear(o); CHECK(ch==1 && gr64(0x208)==0x400,"inline bitmap: changed %d",ch);
  /* inline flags cannot hold more than 128 bits: fail closed */
  put64(0x208,0x500); put32(o+0x50,129); put32(o+0x30,1); put32(o+0x34,256);
  ch=ei_clear(o); CHECK(ch==0 && gr64(0x208)==0x500,"no heap pointer past 128 bits: changed %d",ch);
  /* a target of another class: nothing written */
  put32(o+0x50,1); put64(0x500,0x777);
  ch=ei_clear(o); CHECK(ch==0 && gr64(0x208)==0x500,"different classes: changed %d",ch);
  ei_tests();
  if(fails){ printf("%d failed\n",fails); return 1; }
  printf("ok\n"); return 0;
}
'''


C_PROMPT_TEST = PRELUDE + r'''
/* The action prompt stand-in (main+0x40c5020 in the game): writes what the test says to *pad (x2) the way the game does, plus the
   keyboard outputs (*x3, *x4, the label FKey at x5), returns g_ret, clobbers caller-saved registers and declares the whole asset
   (object and maps) dead when it returns, so a wrapper read of it after the call counts as stale. */
enum { ASSET=0x1000, ODATA=0x2000, IDATA=0x3000, PAD=0x800, KBD=0x808, LABEL=0x810, LKEY=0x818 };
static u64 g_pad_write; static int g_ret, g_calls;
void F_PROMPT(CPU* RESTRICT c, u64 e){ (void)e; g_calls++;
  if(g_pad_write) put64(c->x[2],g_pad_write);
  put64(c->x[3],0x5555); g_mem[c->x[4]]=1; for(int i=0;i<0x18;i++) g_mem[c->x[5]+i]=(u8)(0xa0+i);
  for(int i=1;i<18;i++) c->x[i]=0xc0ffee00+i;
  c->x[0]=(u64)g_ret; g_dead_lo=ASSET; g_dead_hi=0x4000; c->pc=c->x[30]; }
GENERATED

static int fails;
#define CHECK(cond,...) do{ if(!(cond)){ fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } }while(0)
/* InputIcons with the game's layout: key k's 0x70-byte element at ODATA+0x70*k, its platform map's 0x18-byte elements at
   IDATA+0x100*k and, for a hash size above 1, its heap hash at IDATA+0x100*k+0x80. */
typedef struct { u8 plat; u64 tex; } PE;
enum { NKEYS=10 };
static void key_at(int k,const PE* pe,int n,u32 hs){
  u64 el=ODATA+0x70ull*k, id=IDATA+0x100ull*k, hash= hs>1? id+0x80 : el+0x50;
  memset(GP(el),0,0x70); memset(GP(id),0,0x100); put64(el,0x100+k);
  put64(el+0x18,id); put32(el+0x20,n); put32(el+0x24,n); put32(el+0x28,(1u<<n)-1); put32(el+0x40,n); put32(el+0x44,128);
  put32(el+0x48,0xffffffffu); put32(el+0x4c,0); put64(el+0x58, hs>1? hash : 0); put32(el+0x60,hs);
  for(u32 b=0;b<hs;b++) put32(hash+4*b,0xffffffffu);
  for(int i=0;i<n;i++){ u64 x=id+0x18ull*i; u32 b=pe[i].plat&(hs-1);
    put32(x,pe[i].plat); put64(x+8,pe[i].tex); put32(x+0x10,gr32(hash+4*b)); put32(x+0x14,b); put32(hash+4*b,(u32)i); }
  put32(el+0x68,0xffffffffu); }
enum { SW_A=0x7001, PS_A=0x7101, XB_A=0x7201, KB=0x7400, SW_B=0x7002, PS_B=0x7102, SW_C=0x7003, SW_D=0x7004, SW_E=0x7005, PS_E=0x7105,
       PS_E2=0x7106, SW_F=0x7006, PS_F=0x7107 };
static void asset_init(void){
  memset(g_mem,0,sizeof g_mem);
  { PE p[]={{3,SW_A},{1,PS_A},{2,XB_A}}; key_at(0,p,3,1); }        /* inline hash */
  { PE p[]={{0,KB}}; key_at(1,p,1,1); }                            /* a keyboard key: no Switch entry */
  { PE p[]={{0,0x7300},{3,SW_B},{1,PS_B}}; key_at(2,p,3,8); }      /* heap hash, no Xbox entry */
  { PE p[]={{3,SW_A},{1,0x7999}}; key_at(3,p,2,1); }               /* removed below: would make SW_A ambiguous if read */
  { PE p[]={{3,SW_C},{1,0}}; key_at(4,p,2,1); }                    /* NULL PlayStation texture */
  { PE p[]={{3,SW_D}}; key_at(5,p,1,1); }                          /* Switch only */
  { PE p[]={{3,SW_E},{1,PS_E}}; key_at(6,p,2,1); }                 /* 6 and 7: one Switch texture, two replacements */
  { PE p[]={{3,SW_E},{1,PS_E2}}; key_at(7,p,2,1); }
  { PE p[]={{3,SW_F},{1,PS_F}}; key_at(8,p,2,1); }                 /* 8 and 9: one Switch texture, the same replacement */
  { PE p[]={{3,SW_F},{1,PS_F},{2,0x7206}}; key_at(9,p,3,8); }
  put64(ASSET,0xbeef); put64(ASSET+0x30,ODATA); put32(ASSET+0x38,NKEYS); put32(ASSET+0x3c,16);
  put32(ASSET+0x40,((1u<<NKEYS)-1)&~(1u<<3)); put64(ASSET+0x50,0); put32(ASSET+0x58,NKEYS); put32(ASSET+0x5c,128);
  put32(ASSET+0x60,3); put32(ASSET+0x64,1); put32(ASSET+0x68,0); put64(ASSET+0x70,0); put32(ASSET+0x78,1); put64(ASSET+0x80,0x7777);
}
static GuestFn disp[0x20];
static CPU g_cpu; static u8 g_mem_after[sizeof g_mem];
/* one prompt through caller A (0) or B (1); hooked: the wrapper in the dispatch table. Returns *pad; g_cpu / g_mem hold the result */
static u64 run1(int caller,u64 pad_before,u64 write,int want,int hooked,u64 asset){
  CPU* c=&g_cpu; memset(c,0,sizeof *c); c->mem=g_mem; c->disp=disp; c->code_lo=BASE; c->code_hi=BASE+0x80;
  memset(disp,0,sizeof disp); disp[0]=F_CALLA; disp[0x20/4]=F_CALLB; disp[PROMPT/4]= hooked? icon_prompt : F_PROMPT;
  icon_prompt_orig=F_PROMPT; g_icon_want=want; g_icon_base=BASE;
  put64(PAD,pad_before); put64(KBD,0); g_mem[LABEL]=0; memset(GP(LKEY),0,0x18);
  c->x[0]=0x9000; c->x[1]=asset; c->x[2]=PAD; c->x[3]=KBD; c->x[4]=LABEL; c->x[5]=LKEY; c->x[6]=0x9100; c->x[31]=0x4000;
  g_pad_write=write; g_ret= write? 1 : 0; g_calls=0; g_stale=0; g_dead_lo=g_dead_hi=0;
  (caller? F_CALLB : F_CALLA)(c, caller? BASE+0x20 : BASE);
  g_dead_lo=g_dead_hi=0;
  CHECK(g_calls==1,"original called once (%d)",g_calls);
  CHECK(c->pc==BASE+(caller? 0x24 : 4),"returned to the caller (pc %llx)",(unsigned long long)c->pc);
  CHECK(g_stale==0,"%d reads of the asset after the original returned",g_stale);
  return gr64(PAD);
}
/* the same prompt unhooked and hooked: registers and guest memory equal except *pad, which must be expect */
static void same(const char* what,int caller,u64 before,u64 write,int want,u64 expect){
  asset_init(); u64 r0=run1(caller,before,write,want,0,ASSET); CPU c0=g_cpu; memcpy(g_mem_after,g_mem,sizeof g_mem);
  asset_init(); u64 r1=run1(caller,before,write,want,1,ASSET);
  CHECK(r0==(write? write : before),"%s: unhooked pad %llx",what,(unsigned long long)r0);
  CHECK(r1==expect,"%s: pad %llx, expected %llx",what,(unsigned long long)r1,(unsigned long long)expect);
  CHECK(!memcmp(&c0,&g_cpu,sizeof c0),"%s: registers differ from the original's",what);
  put64(PAD,r0);
  CHECK(!memcmp(g_mem,g_mem_after,sizeof g_mem),"%s: guest memory other than the pad slot changed",what);
}
/* a malformed map: refused for the expected reason, the Switch texture stays, no crash */
static void bad(const char* what,void (*spoil)(void),const char* reason){
  asset_init(); spoil();
  IconPair t[ICON_PAIRS]; int np=0; const char* why=icon_pairs(ASSET,1,t,&np);
  CHECK(why && !strcmp(why,reason),"%s: refused as '%s', expected '%s'",what,why?why:"(accepted)",reason);
  u64 r=run1(0,0,SW_A,1,1,ASSET);
  CHECK(r==SW_A,"%s: pad %llx, expected the Switch texture kept",what,(unsigned long long)r);
}
static void sp_bits(void){ put32(ASSET+0x58,NKEYS+1); }
static void sp_count(void){ put32(ASSET+0x38,5000); put32(ASSET+0x58,5000); }
static void sp_free(void){ put32(ASSET+0x64,NKEYS+1); }
static void sp_live(void){ put32(ASSET+0x64,2); }                           /* one live key fewer than the flags say */
static void sp_index(void){ put32(ODATA+0x50,40); }                          /* key 0's hash bucket points past its 3 entries */
/* key 2 (hash size 8): bucket 3 (Switch) starts at the Desktop entry (index 0), which chains to itself */
static void sp_cycle(void){ put32(IDATA+0x280+12,0); put32(IDATA+0x200+0x10,0); }
static void sp_hs(void){ put32(ODATA+0xe0+0x60,6); }                         /* key 2's hash size is not a power of two */
static void sp_inline_hs(void){ put32(ODATA+0x60,2); }                       /* inline hash (no heap) with two buckets */
static void sp_inner_free(void){ put32(ODATA+0x4c,4); }                      /* key 0: more free than entries */
static void sp_heap_bits(void){ put32(ASSET+0x58,200); put32(ASSET+0x38,200); } /* past the inline flags without a heap pointer */
static void sp_data(void){ put64(ASSET+0x30,0x2004); }                        /* misaligned element array */
int main(void){
  same("PlayStation through caller A",0,0,SW_A,1,PS_A);
  same("PlayStation through caller B",1,0,SW_A,1,PS_A);
  same("Xbox through caller B",1,0,SW_A,2,XB_A);
  same("heap hash",0,0,SW_B,1,PS_B);
  same("missing Xbox entry keeps Switch",1,0,SW_B,2,SW_B);
  same("NULL PlayStation texture keeps Switch",0,0,SW_C,1,SW_C);
  same("Switch-only key keeps Switch",0,0,SW_D,1,SW_D);
  same("ambiguous Switch texture keeps Switch",0,0,SW_E,1,SW_E);
  same("one Switch texture, one replacement",1,0,SW_F,1,PS_F);
  same("no gamepad icon written",0,0,0,1,0);
  same("slot unchanged (stale caller value)",0,SW_A,0,1,SW_A);
  same("not an icon of this asset",0,0,0x6666,1,0x6666);
  same("NULL asset passes through",0,0,0,1,0);
  { asset_init(); u64 r=run1(0,0,SW_A,1,1,0); CHECK(r==SW_A,"NULL asset: pad %llx",(unsigned long long)r); }
  { asset_init(); u64 r=run1(0,0,SW_A,1,1,ASSET+4); CHECK(r==SW_A,"misaligned asset: pad %llx",(unsigned long long)r); }
  bad("bit count differs from the element count",sp_bits,"key map counts"); bad("too many keys",sp_count,"key map counts");
  bad("free count",sp_free,"key map counts"); bad("live count",sp_live,"key map live count");
  bad("hash index out of range",sp_index,"platform map"); bad("hash chain cycle",sp_cycle,"platform map");
  bad("hash size",sp_hs,"platform map"); bad("inline hash with two buckets",sp_inline_hs,"platform map");
  bad("inner free count",sp_inner_free,"platform map"); bad("flags past the inline words",sp_heap_bits,"key map flags");
  bad("misaligned elements",sp_data,"key map not readable");
  { asset_init(); IconPair t[ICON_PAIRS]; int np=0; const char* why=icon_pairs(ASSET,1,t,&np);
    CHECK(!why && np==6,"well-formed map: %s, %d Switch textures",why?why:"accepted",np); }
  for(int i=0;i<40;i++) same("repeated",i&1,0,SW_A,1,PS_A);   /* more prompts than the trace keeps */
  printf("trace %d logs %d last: %s\n",g_ntrace,g_nlog,g_trace);
  if(fails){ printf("%d failed\n",fails); return 1; }
  printf("ok\n"); return 0;
}
'''


class ButtonLayoutRuntimeTests(unittest.TestCase):
    def build_and_run(self, dynamic):
        cc = os.environ.get('CC_HOST') or shutil.which('gcc') or shutil.which('clang')   # not 'cc': a launcher on some hosts
        if not cc: self.skipTest('no C compiler')
        prog, caller, funcs = fixture_program(dynamic)
        gen = codegen.emit_function(prog, caller, funcs)
        gen = gen.replace(caller.name, 'F_CALLER').replace(funcs[BASE + LOOKUP].name, 'F_LOOKUP')
        src = C_TEST.replace('GENERATED', 'enum { BASE=0x%X, LOOKUP=0x%X };\nvoid F_CALLER(CPU* RESTRICT c, u64 e);\n%s'
                             % (BASE, LOOKUP, gen))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 't.c'); exe = os.path.join(d, 't')
            with open(p, 'w') as f: f.write(src)
            b = subprocess.run([cc, '-O1', '-w', '-I', os.path.join(ROOT, 'runtime'), p, '-o', exe], capture_output=True, text=True)
            self.assertEqual(b.returncode, 0, b.stderr[-3000:])
            return subprocess.run([exe], capture_output=True, text=True)

    def build_prompt_and_run(self, dynamic, env=None):
        cc = os.environ.get('CC_HOST') or shutil.which('gcc') or shutil.which('clang')
        if not cc: self.skipTest('no C compiler')
        prog, callers, funcs = prompt_fixture_program(dynamic)
        gen = ''.join(codegen.emit_function(prog, f, funcs) for f in callers)
        gen = (gen.replace(callers[0].name, 'F_CALLA').replace(callers[1].name, 'F_CALLB')
               .replace(funcs[BASE + PROMPT].name, 'F_PROMPT'))
        src = C_PROMPT_TEST.replace('GENERATED', 'enum { BASE=0x%X, PROMPT=0x%X };\nvoid F_CALLA(CPU* RESTRICT c, u64 e);\n'
                                    'void F_CALLB(CPU* RESTRICT c, u64 e);\n%s' % (BASE, PROMPT, gen))
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 't.c'); exe = os.path.join(d, 't')
            with open(p, 'w') as f: f.write(src)
            b = subprocess.run([cc, '-O1', '-w', '-I', os.path.join(ROOT, 'runtime'), p, '-o', exe], capture_output=True, text=True)
            self.assertEqual(b.returncode, 0, b.stderr[-3000:])
            e = {k: v for k, v in os.environ.items() if k != 'SWR_ICON_TRACE'}
            e.update(env or {})
            return subprocess.run([exe], capture_output=True, text=True, env=e)

    def test_action_prompt_gets_the_layout_icon_of_the_same_key(self):
        r = self.build_prompt_and_run(True)
        self.assertEqual(r.returncode, 0, r.stdout)
        m = re.search(r'trace (\d+) logs (\d+)', r.stdout)
        self.assertEqual(int(m.group(1)), 0, 'no trace without SWR_ICON_TRACE')
        self.assertLessEqual(int(m.group(2)), 1, 'at most the one-time "keeps its Switch icon" line: ' + r.stdout)

    def test_action_prompt_trace_is_opt_in_and_bounded(self):
        r = self.build_prompt_and_run(True, {'SWR_ICON_TRACE': '1'})
        self.assertEqual(r.returncode, 0, r.stdout)
        m = re.search(r'trace (\d+) logs \d+ last: (.*)', r.stdout)
        self.assertEqual(int(m.group(1)), 32, r.stdout)   # more than 32 prompts ran: the trace stops at 32
        self.assertRegex(m.group(2), r'caller main\+(4|24),')

    def test_direct_generated_prompt_call_bypasses_the_wrapper(self):
        r = self.build_prompt_and_run(False)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('FAIL: PlayStation through caller A: pad 7001', r.stdout)
        self.assertIn('FAIL: PlayStation through caller B: pad 7001', r.stdout)

    def test_hook_reached_from_generated_code_with_fallbacks(self):
        r = self.build_and_run(True)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_redirect_wrapper_rewrites_before_the_call_and_reads_nothing_after(self):
        r = self.build_and_run(True)
        for name in ('install:', 'original called once', 'rewritten before the original', 'guest reads after the object was retired',
                     'map structure kept', 'live object rewritten', 'unrelated object unchanged', 'empty map', 'non-object'):
            self.assertNotIn('FAIL: ' + name, r.stdout)
        self.assertEqual(r.returncode, 0, r.stdout)

    def test_direct_generated_call_bypasses_the_hook(self):
        r = self.build_and_run(False)   # what 90ec448 shipped: the wrapper in g_disp is never reached
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('PlayStation texture through the generated call', r.stdout)


if __name__ == '__main__':
    unittest.main()
