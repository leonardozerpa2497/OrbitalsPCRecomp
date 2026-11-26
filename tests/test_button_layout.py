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


class CodegenDynamicCallTests(unittest.TestCase):
    def test_orbitals_icon_lookup_is_listed_for_the_runtimes_build(self):
        self.assertEqual(codegen.DYNAMIC_CALLS[ORBITALS_MAIN], (0x40c5ce0,))
        with open(os.path.join(ROOT, 'runtime', 'orbitals_input.c')) as f: src = f.read()
        build = re.search(r'orbitals_build\[16\] = \{([^}]*)\}', src).group(1)
        self.assertEqual(bytes(int(x, 16) for x in build.split(',')).hex(), ORBITALS_MAIN)
        self.assertIn('ICON_LOOKUP=0x40c5ce0', src)

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


C_TEST = r'''
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
static u8 g_mem[0x10000];
GuestFn swr_interp_entry;
#define GP(a) (g_mem+(u64)(a))
static int g_retired; static int g_stale;   /* guest reads made after a callee retired the object */
static u64 gr64(u64 a){ u64 v; if(g_retired) g_stale++; memcpy(&v,GP(a),8); return v; }
static u32 gr32(u64 a){ u32 v; if(g_retired) g_stale++; memcpy(&v,GP(a),4); return v; }
static void put64(u64 a,u64 v){ memcpy(GP(a),&v,8); }
static void put32(u64 a,u32 v){ memcpy(GP(a),&v,4); }
#define LOG(...) ((void)0)
enum { PERM_R=1, PERM_RW=3 };
static int mem_span_ok_locked(u64 a,u64 n,u32 p){ (void)p; return a+n<=sizeof g_mem; }
static int guest_span_readable(u64 a,u64 n,u64 al){ return !(a&(al-1)) && a+n<=sizeof g_mem; }
static int g_locked;
static int mem_with_range(u64 a,u64 n,void (*fn)(u64,u64,void*),void* ctx){ if(!guest_span_readable(a,n,1)) return 0;
  g_locked++; fn(a,n,ctx); g_locked--; return 1; }
static u64 g_ei_vtable;
typedef struct CPU CPU;
#include "orbitals_buttons.h"

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
