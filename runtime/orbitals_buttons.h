/* Orbitals PC Recomp, based on SwitchRecomp by MZ Labs. AGPL-3.0, see LICENSE.txt and NOTICE.txt. */
/* Button layout pieces that only touch the CPU state and guest memory, shared by runtime/orbitals_input.c and
   tests/test_button_layout.py. The includer provides CPU, GuestFn, GP, gr32, gr64, LOG, PERM_R / PERM_RW,
   guest_span_readable, mem_with_range, g_ei_vtable and mem_span_ok_locked (inside mem_with_range only). */
#ifndef ORBITALS_BUTTONS_H
#define ORBITALS_BUTTONS_H

/* The icon lookup, main+0x40c5ce0 (asset, const FKey*, EPlatformType, UTexture2D** out) -> bool, returns with c->pc = x30.
   It writes *out only when the key has an entry for the platform, and that entry's texture may be NULL (true, *out = NULL).
   A Switch request (3) and, for PlayStation, the native widget's Xbox fallback (2) ask for the layout's platform first; when that
   finds no entry or a NULL texture, *out and the arguments are put back and the original platform is asked. */
static GuestFn icon_lookup_orig; static int g_icon_want;   /* 1 Playstation, 2 Xbox */
static u64 g_icon_base;   /* main's base, only to print callers as main+offset */
/* SWR_ICON_TRACE (environment, read once): opt-in, bounded evidence of which caller reaches which wrapper; installing a wrapper does
   not prove a prompt goes through it. Off by default: no logging on these paths. */
static int icon_trace(void){
  static int trace=-1;
  int t=__atomic_load_n(&trace,__ATOMIC_RELAXED);
  if(t<0){ t=getenv("SWR_ICON_TRACE")!=NULL; __atomic_store_n(&trace,t,__ATOMIC_RELAXED); }
  return t;
}
static void icon_lookup(CPU* c, u64 entry){
  u8 plat=(u8)c->x[2];
  static unsigned traced;
  if(icon_trace() && __atomic_fetch_add(&traced,1,__ATOMIC_RELAXED)<16)
    LOG("Orbitals: icon lookup reached: platform %u, wanted %d, caller main+%llx\n",plat,g_icon_want,(unsigned long long)(c->x[30]-g_icon_base));
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

/* The action prompt, main+0x40c5020 (UInputAction*, asset, UTexture2D** pad, UTexture2D** keyboard, bool* label, FKey* label key,
   player) -> bool: the path the title / menus / lobby / settings / co-op prompts actually take (both callers, main+0x40c4ed0 and
   main+0x40d3990, reach it with a direct bl). For each key mapped to the action it looks the key up in the asset's InputIcons itself
   (the same two-level map as icon_lookup, inlined) with the platform hard-coded: a gamepad key asks Switch 3 and writes the texture
   to *pad; a keyboard key asks Desktop 0 and writes *keyboard, or, without one, the asset's default texture, *label = true and a copy
   of the key into *label key (so that FKey is the keyboard key, never the gamepad one). Nothing it returns names the gamepad key.
   icon_prompt therefore maps the Switch texture it wrote back to the layout's texture of the SAME key through the asset's own map:
   before the call (the asset is the caller's argument, live exactly as long as the original's own reads of it) it lists, for every
   live key with a non-NULL Switch texture, that key's PlayStation / Xbox texture; after the call it reads only *pad, the caller's
   output slot. A Switch texture whose keys disagree (different, missing or NULL replacements), a malformed map or a slot the call did
   not change keep the Switch texture. Registers, the return value, *keyboard, *label, the label key (and its reference count) are
   the original's: the only write is *pad, and only to a texture of the same asset. */
enum { ICON_PAIRS=256, ICON_KEYS_MAX=4096, ICON_PLATS_MAX=64 };
typedef struct { u64 sw, rep; } IconPair;   /* rep 0: no usable replacement (missing, NULL or ambiguous) */
/* A TSparseArray's allocation flags (TBitArray, 4 inline words, heap pointer, bit count, bit capacity) checked against its element count
   num, capacity max and free count nfree: bits == num <= max <= limit, bit capacity >= bits (<= 128 without a heap pointer), the words
   readable, and exactly num - nfree bits set. The flag words, or 0 when any of that fails (*live_bad: only the set bits disagree). */
static u64 icon_flags(u64 inl, u64 heap, u32 bits, u32 maxbits, u32 num, u32 max, u32 nfree, u32 limit, int* live_bad){
  *live_bad=0;
  if(max>limit || num>max || nfree>num || bits!=num || maxbits<bits) return 0;
  if(!heap && maxbits>128) return 0;
  u64 words= heap? heap : inl;
  if((words&3) || !guest_span_readable(words,(bits+31)/32*4ull,4)) return 0;
  u32 live=0;
  for(u32 i=0;i<bits;i++) live+=gr32(words+4*(i/32))>>(i%32)&1;
  if(live!=num-nfree){ *live_bad=1; return 0; }
  return words;
}
static int icon_live(u64 words, u32 i){ return gr32(words+4*(i/32))>>(i%32)&1; }
/* One key's TMap<EPlatformType, UTexture2D*> inside its 0x70-byte InputIcons element el: data +0x18, count +0x20, capacity +0x24,
   allocation flags (inline words +0x28, heap +0x38, bit count +0x40, bit capacity +0x44), free count +0x4c, hash inline +0x50 / heap
   +0x58, hash size +0x60; 0x18-byte elements {platform byte, texture +8, hash next +0x10}. Looked up through the hash exactly as the
   game does (bucket = platform & (size-1)); every element the chain reaches must be allocated. 1 found (*tex may be NULL), 0 no
   entry, -1 malformed. */
static int icon_plat_find(u64 el, u8 plat, u64* tex){
  u64 data=gr64(el+0x18), hash=gr64(el+0x58);
  u32 num=gr32(el+0x20), max=gr32(el+0x24), nfree=gr32(el+0x4c), hs=gr32(el+0x60);
  int live_bad;
  u64 words=icon_flags(el+0x28,gr64(el+0x38),gr32(el+0x40),gr32(el+0x44),num,max,nfree,ICON_PLATS_MAX,&live_bad);
  if(!words) return -1;
  if(num==nfree) return 0;
  if(!hs || hs>ICON_PLATS_MAX || (hs&(hs-1))) return -1;
  if(!hash){ if(hs!=1) return -1; hash=el+0x50; }
  if(!data || (data&7) || (hash&3) || !guest_span_readable(data,0x18ull*num,8) || !guest_span_readable(hash,4ull*hs,4)) return -1;
  int32_t i=(int32_t)gr32(hash+4ull*(plat&(hs-1)));
  for(u32 steps=0; i!=-1; steps++){
    if(i<0 || (u32)i>=num || steps>=num || !icon_live(words,(u32)i)) return -1;   /* out of range, a cycle, or a freed element */
    u64 e=data+0x18ull*(u32)i;
    if((u8)gr32(e)==plat){ *tex=gr64(e+8); return 1; }
    i=(int32_t)gr32(e+0x10);
  }
  return 0;
}
/* InputIcons at asset+0x30 (TMap<FKey, entry>): data +0x30, count +0x38, capacity +0x3c, allocation flags (inline words +0x40, heap
   +0x50, bit count +0x58, bit capacity +0x5c), free count +0x64; 0x70-byte elements. Lists {Switch texture -> layout texture} for every
   live key into t (*np pairs). NULL when the list is usable, else why not (then nothing is translated). */
static const char* icon_pairs(u64 asset, u8 want, IconPair* t, int* np){
  *np=0;
  if((asset&7) || !guest_span_readable(asset,0x80,8)) return "asset not readable";
  u64 data=gr64(asset+0x30), heap=gr64(asset+0x50);
  u32 num=gr32(asset+0x38), max=gr32(asset+0x3c), bits=gr32(asset+0x58), maxbits=gr32(asset+0x5c), nfree=gr32(asset+0x64);
  if(max>ICON_KEYS_MAX || num>max || nfree>num || bits!=num || maxbits<bits) return "key map counts";
  if(num==nfree) return NULL;
  if(!heap && (bits>128 || maxbits>128)) return "key map flags";
  if(!data || (data&7) || !guest_span_readable(data,0x70ull*num,8)) return "key map not readable";
  int live_bad;
  u64 words=icon_flags(asset+0x40,heap,bits,maxbits,num,max,nfree,ICON_KEYS_MAX,&live_bad);
  if(!words) return live_bad? "key map live count" : "key map flags";
  for(u32 i=0;i<bits;i++){
    if(!icon_live(words,i)) continue;
    u64 el=data+0x70ull*i, sw=0, rep=0;
    int f=icon_plat_find(el,3,&sw); if(f<0) return "platform map";
    if(!f || !sw) continue;
    int g=icon_plat_find(el,want,&rep); if(g<0) return "platform map";
    if(!g) rep=0;
    int k=0; while(k<*np && t[k].sw!=sw) k++;
    if(k<*np){ if(t[k].rep!=rep) t[k].rep=0; continue; }   /* one Switch texture, keys that disagree: ambiguous, kept */
    if(*np>=ICON_PAIRS) return "too many icons";
    t[k].sw=sw; t[k].rep=rep; (*np)++;
  }
  return NULL;
}
static GuestFn icon_prompt_orig;
static void icon_prompt(CPU* c, u64 entry){
  u64 action=c->x[0], asset=c->x[1], pad=c->x[2], lr=c->x[30];
  static unsigned traced; static int said;
  int tracing= icon_trace() && __atomic_fetch_add(&traced,1,__ATOMIC_RELAXED)<32;
  /* the original returns false at once, writing nothing, without an action or an asset */
  if(!action || !asset || (pad&7) || !guest_span_readable(pad,8,8)){
    if(tracing) LOG("Orbitals: icon prompt reached: caller main+%llx, action %llx, asset %llx, pad slot %llx: passed through\n",
                    (unsigned long long)(lr-g_icon_base),(unsigned long long)action,(unsigned long long)asset,(unsigned long long)pad);
    icon_prompt_orig(c,entry); return; }
  IconPair t[ICON_PAIRS]; int np=0;
  u64 before=gr64(pad);
  const char* why=icon_pairs(asset,(u8)g_icon_want,t,&np);
  icon_prompt_orig(c,entry);
  /* after the call: only the caller's pad slot is read (and written) */
  u64 after=gr64(pad), rep=0;
  if(!why){
    if(after==before || !after) why= after? "pad slot unchanged" : NULL;   /* no gamepad icon written (or a NULL one): nothing to do */
    else{ int k=0; while(k<np && t[k].sw!=after) k++;
      if(k==np) why="not a Switch icon of this asset"; else if(!t[k].rep) why="no single icon for this key"; else rep=t[k].rep; }
  }
  if(rep) memcpy(GP(pad),&rep,8);
  if(tracing) LOG("Orbitals: icon prompt reached: caller main+%llx, action %llx, asset %llx: pad icon %llx -> %llx%s%s\n",
                  (unsigned long long)(lr-g_icon_base),(unsigned long long)action,(unsigned long long)asset,(unsigned long long)after,
                  (unsigned long long)(rep? rep : after),why?", kept: ":"",why?why:"");
  if(why && after && !__atomic_exchange_n(&said,1,__ATOMIC_RELAXED))
    LOG("Orbitals: button icons: a prompt keeps its Switch icon (%s)\n",why);
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
