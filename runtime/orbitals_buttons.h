/* Orbitals PC Recomp, based on SwitchRecomp by MZ Labs. AGPL-3.0, see LICENSE.txt and NOTICE.txt. */
/* Button layout pieces that only touch the CPU state and guest memory, shared by runtime/orbitals_input.c and
   tests/test_button_layout.c. The includer provides CPU, GuestFn, GP, gr32, gr64, LOG, PERM_R / PERM_RW,
   guest_span_readable, mem_with_range, g_ei_vtable and mem_span_ok_locked (inside mem_with_range only). */
#ifndef ORBITALS_BUTTONS_H
#define ORBITALS_BUTTONS_H

/* The icon lookup, main+0x40c5ce0 (asset, const FKey*, EPlatformType, UTexture2D** out) -> bool, returns with c->pc = x30.
   It writes *out only when the key has an entry for the platform, and that entry's texture may be NULL (true, *out = NULL).
   A Switch request (3) and, for PlayStation, the native widget's Xbox fallback (2) ask for the layout's platform first; when that
   finds no entry or a NULL texture, *out and the arguments are put back and the original platform is asked. */
static GuestFn icon_lookup_orig; static int g_icon_want;   /* 1 Playstation, 2 Xbox */
static void icon_lookup(CPU* c, u64 entry){
  u8 plat=(u8)c->x[2];
  if(!(plat==3 || (plat==2 && g_icon_want==1))){ icon_lookup_orig(c,entry); return; }
  u64 x0=c->x[0], x1=c->x[1], x3=c->x[3], lr=c->x[30], out0=gr64(x3);
  c->x[2]=(u64)g_icon_want;
  icon_lookup_orig(c,entry);
  if((c->x[0]&1) && gr64(x3)) return;
  static int said; if(!__atomic_exchange_n(&said,1,__ATOMIC_RELAXED)) LOG("Orbitals: button icons: a key without a %s icon keeps the platform %d one\n",g_icon_want==1?"PlayStation":"Xbox",plat);
  memcpy(GP(x3),&out0,8);
  c->x[0]=x0; c->x[1]=x1; c->x[2]=plat; c->x[3]=x3; c->x[30]=lr;
  icon_lookup_orig(c,entry);
}

/* UEnhancedInputPlatformData::MappingContextRedirects, a TMap at obj+0x28: TSparseArray data +0x28, count +0x30, capacity +0x34,
   allocation flags TBitArray (4 inline words +0x38, heap pointer +0x48, bit count +0x50, bit capacity +0x54), first free +0x58, free
   count +0x5c. The bit array uses the heap pointer whenever it is set (its capacity outgrew the inline words), whatever the bit count. */
static int ei_clear(u64 obj){
  if((obj&7) || !mem_span_ok_locked(obj,0x78,PERM_RW)) return 0;
  u64 data=gr64(obj+0x28);
  u32 num=gr32(obj+0x30), max=gr32(obj+0x34), bits=gr32(obj+0x50), nfree=gr32(obj+0x5c);
  if(!num || num>256 || max<num || bits<num || bits>max+64 || nfree>num || (data&7)) return 0;
  u64 heap=gr64(obj+0x48), words= heap? heap : obj+0x38;
  if(!heap && bits>128) return 0;
  if(!data || (words&3) || !mem_span_ok_locked(data,24ull*bits,PERM_RW) || !mem_span_ok_locked(words,(bits+31)/32*4ull,PERM_R)) return 0;
  /* every live element {key, value, hash next, hash index}: value := key, so each redirect points a context to itself. One aligned
     8-byte store per element (a reader sees the old or the new context, both live objects); count, free list and hash stay as they are */
  int changed=0;
  for(u32 i=0;i<bits;i++){
    if(!(gr32(words+4*(i/32))>>(i%32)&1)) continue;
    u64 e=data+24ull*i, k=gr64(e), v=gr64(e+8);
    if(!k || !v || k==v) continue;
    if((k&7) || (v&7) || !mem_span_ok_locked(k,8,PERM_R) || !mem_span_ok_locked(v,8,PERM_R) || gr64(k)!=gr64(v)) return changed;   /* two mapping contexts or nothing */
    __atomic_store_n((u64*)GP(e+8),k,__ATOMIC_RELEASE); changed++;
  }
  if(changed) LOG("Orbitals: button layout: %d Switch mapping context redirects of the platform data at %llx now keep the PC contexts\n",changed,(unsigned long long)obj);
  return changed>0; }

/* The redirect maps are rewritten before their first use: the virtual functions the object's vtable reaches (UObject's own, shared
   with other classes) go through ei_virtual, which rewrites the map of the UEnhancedInputPlatformData it is called on BEFORE calling the
   original, while the caller still holds the object (its receiver, x0): the loader calls several of them (PostLoad and friends) after
   reading the object's properties and before Enhanced Input reads the map, and a call made while the map is still empty (Serialize)
   finds nothing to do. Nothing reads the object after the original returns, so a callee that retires or frees it (a destructor, a
   ConditionalBeginDestroy chain, a GC hook) cannot make the wrapper touch a dead object; the original gets the registers unchanged.
   Slots 0 and 1 are the class's complete and deleting destructors (main+0x1684900 / 0x16849b0, the only two entries no other vtable
   shares; the deleting one frees the object), so they are never wrapped. */
enum { EI_SLOTS=64, EI_FIRST_SLOT=2 };
static struct { u64 pc; GuestFn orig; } g_eiw[EI_SLOTS]; static int g_neiw;
static void ei_clear_cb(u64 a, u64 n, void* ctx){ (void)n; (void)ctx; ei_clear(a); }
static int ei_is_data(u64 obj){ return obj && !(obj&7) && guest_span_readable(obj,0x78,8) && gr64(obj)==g_ei_vtable; }
static void ei_virtual(CPU* c, u64 entry){
  GuestFn orig=NULL; for(int i=0;i<g_neiw;i++) if(g_eiw[i].pc==entry){ orig=g_eiw[i].orig; break; }
  if(!orig) return;
  u64 obj=c->x[0];
  if(ei_is_data(obj) && gr32(obj+0x30)) mem_with_range(obj,0x78,ei_clear_cb,NULL);
  orig(c,entry);   /* tail position: no read of obj or c after it */
}
/* Wraps the entries of vtable [slot EI_FIRST_SLOT, end) in disp (indexed (pc-code_lo)/4). Vtable words below mod_size are module
   offsets (the game's loader relocates its data later). Stops at the first word outside the code; skips the destructors, duplicates,
   entries without code and entries already wrapped. Returns the number of wrapped functions. */
static int ei_wrap_vtable(u64 vtable, u64 mod_base, u64 mod_size, u64 code_lo, u64 code_hi, GuestFn* disp){
  u64 dtor[2]={0,0};
  for(int i=0;i<EI_SLOTS && g_neiw<EI_SLOTS;i++){
    u64 slot=vtable+8ull*i; if(!guest_span_readable(slot,8,8)) break;
    u64 fn=gr64(slot); if(fn<mod_size) fn+=mod_base;
    if(fn<code_lo || fn>=code_hi || (fn&3)) break;   /* past the end of the vtable */
    if(i<EI_FIRST_SLOT){ dtor[i]=fn; continue; }   /* the destructors */
    if(fn==dtor[0] || fn==dtor[1]) continue;
    int dup=0; for(int k=0;k<g_neiw;k++) if(g_eiw[k].pc==fn) dup=1;
    GuestFn* e=&disp[(fn-code_lo)/4];
    if(dup || !*e || *e==ei_virtual) continue;
    g_eiw[g_neiw].pc=fn; g_eiw[g_neiw].orig=*e; g_neiw++; *e=ei_virtual;
  }
  return g_neiw;
}

#endif
