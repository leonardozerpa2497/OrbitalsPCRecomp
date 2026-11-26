/* The gyro gate (runtime/orbitals_input.c) driven end to end: pad samples through orbitals_gyro_pad, the hooked UpdateRotation
   on fake guest player controllers, AddYawInput / AddPitchInput and the game's UpdateRotation as small engine stand-ins that turn
   ControlRotation. Build and run: python tests/test_gyro_gate.py (or gcc -std=gnu11 -Iruntime tests/test_gyro_gate.c -lm). */
#include "orbitals_input.c"
#include <stdarg.h>
#include <sys/stat.h>

char g_datadir[1024] = "/nonexistent-orbitals-gyro-gate/";
int plat_mkdir(const char* p){ (void)p; return -1; }
void ini_set(const char* k, const char* v){ (void)k; (void)v; }

static char logbuf[1 << 16]; static size_t loglen;
void logf_(int level, const char* fmt, ...){
  (void)level; va_list ap; va_start(ap, fmt);
  int n = vsnprintf(logbuf + loglen, sizeof logbuf - loglen, fmt, ap); va_end(ap);
  if(n > 0) loglen = loglen + (size_t)n < sizeof logbuf ? loglen + (size_t)n : sizeof logbuf - 1;
}
static int logged(const char* s){ return strstr(logbuf, s) != NULL; }

/* guest memory: 1 MiB, one range can be made unreadable */
enum { MEM = 1 << 20 };
u8* g_mem; static u64 bad_lo, bad_hi;
int guest_span_readable(u64 a, u64 n, u64 al){
  if(a + n > MEM || a + n < a || (al && a % al)) return 0;
  return a + n <= bad_lo || a >= bad_hi;
}
static u64 clock_ns;
/* abort_countdown: the n-th clock read from now ends the test as the game thread would (INCONCLUSIVE under test_lock), to land that
   end exactly where the frontend thread has let go of test_lock inside its synthetic sample loop */
static int abort_countdown, settle_countdown;
u64 plat_ns(void){
  u64 captured=clock_ns;
  if(settle_countdown && --settle_countdown==0){
    clock_ns++;
    orbitals_gyro_pad(0,100,0x054c,0x0ce6,0,0,NULL,0,1,1);
    return captured;
  }
  if(abort_countdown && --abort_countdown == 0){ test_lock(); test_inconclusive("simulated game-thread end during a synthetic sample"); test_unlock(); }
  return clock_ns;
}
GuestFn* g_disp; u64 g_code_lo = 0x80000, g_code_hi = 0x80100;
static double rd(u64 a){ double d; memcpy(&d, GP(a), 8); return d; }
static void wd(u64 a, double d){ memcpy(GP(a), &d, 8); }

/* engine stand-ins: AddYaw/PitchInput add to RotationInput (kept here per controller); UpdateRotation applies and clears it */
enum { NPC = 3 };
static u64 pcs[NPC]; static double rin_yaw[NPC], rin_pitch[NPC]; static int add_calls, orig_calls, engine_drops;
static int pc_index(u64 pc){ for(int i = 0; i < NPC; i++) if(pcs[i] == pc) return i; return -1; }
static void fake_add_yaw(CPU* c, u64 e){ (void)e; rin_yaw[pc_index(c->x[0])] += c->v[0].f[0]; add_calls++; c->pc = c->x[30]; }
static void fake_add_pitch(CPU* c, u64 e){ (void)e; rin_pitch[pc_index(c->x[0])] += c->v[0].f[0]; add_calls++; c->pc = c->x[30]; }
static void fake_update_rotation(CPU* c, u64 e){
  (void)e; int i = pc_index(c->x[0]); orig_calls++;
  if(!engine_drops){ wd(pcs[i] + PC_CONTROL_ROTATION, rd(pcs[i] + PC_CONTROL_ROTATION) + rin_pitch[i]);
                     wd(pcs[i] + PC_CONTROL_ROTATION + 8, rd(pcs[i] + PC_CONTROL_ROTATION + 8) + rin_yaw[i]); }
  rin_yaw[i] = rin_pitch[i] = 0;
}

/* controller i: ControllerId i, its own pawn, acknowledged, viewed through by its camera manager */
static u64 pawn_of(int i){ return 0x30000 + (u64)i * 0x1000; }
static u64 pcm_of(int i){ return 0x40000 + (u64)i * 0x2000; }
static void make_pc(int i, int controller_id){
  u64 pc = 0x10000 + (u64)i * 0x1000, lp = 0x20000 + (u64)i * 0x100;
  pcs[i] = pc; gw64(pc + PC_PLAYER, lp); gw32(lp + LP_CONTROLLER_ID, (u32)controller_id);
  gw64(pc + PC_PAWN, pawn_of(i)); gw64(pc + PC_ACK_PAWN, pawn_of(i)); *GP(pc + PC_IGNORE_LOOK) = 0;
  gw64(pc + PC_CAMERA_MANAGER, pcm_of(i)); gw64(pcm_of(i) + PCM_VIEW_TARGET, pawn_of(i)); gw64(pcm_of(i) + PCM_PENDING_VIEW_TARGET, 0);
}
static double cam_yaw(int i){ return rd(pcs[i] + PC_CONTROL_ROTATION + 8); }
static double cam_pitch(int i){ return rd(pcs[i] + PC_CONTROL_ROTATION); }

static void reset(void){
  memset(g_mem, 0, MEM); memset(P, 0, sizeof P); memset(Acc, 0, sizeof Acc); memset(Gs, 0, sizeof Gs); memset(&T, 0, sizeof T);
  memset(rin_yaw, 0, sizeof rin_yaw); memset(rin_pitch, 0, sizeof rin_pitch);
  add_calls = orig_calls = engine_drops = abort_countdown = settle_countdown = 0; bad_lo = bad_hi = 0; loglen = 0; logbuf[0] = 0;
  G.mode = 1; G.sens_x = G.sens_y = 1.5f; G.tightening = 3; G.invert = 0; G.test = 0; G.log = 0; G.button = 20; G.button_toggle = 0;
  clock_ns = 5000000000000ull;
  make_pc(0, 0); make_pc(1, 1);
}

/* one 16 ms frame: each pad sends 4 samples (250 Hz) turning yaw_dps (a left turn) and pitch_dps (tilt up), then the listed
   controllers run UpdateRotation (none while paused) */
static void tick(int i, float dt){
  CPU c; memset(&c, 0, sizeof c); c.x[0] = pcs[i]; c.v[0].f[0] = dt; c.x[30] = 0xdead;
  update_rotation(&c, UPDATE_ROTATION, fake_update_rotation);
}
static double rate[2][2];   /* [slot][0 yaw, 1 pitch], deg/s */
static void frame(int ticks, float dt){
  for(int s = 0; s < 2; s++){
    PadMotion m[4];
    for(int k = 0; k < 4; k++){
      m[k].t_ns = clock_ns + (u64)k * 4000000ull;
      m[k].gyro[0] = (float)(rate[s][1] / RAD2DEG); m[k].gyro[1] = (float)(rate[s][0] / RAD2DEG); m[k].gyro[2] = 0;
      m[k].accel[0] = 0; m[k].accel[1] = 9.80665f; m[k].accel[2] = 0;
    }
    orbitals_gyro_pad(s, 100 + s, 0x054c, 0x0ce6, 0, 0, m, 4, 1, 1);
  }
  clock_ns += 16000000ull;
  for(int i = 0; i < NPC; i++) if(ticks >> i & 1 && pcs[i]) tick(i, dt);
}
static void frames(int n, int ticks){ for(int k = 0; k < n; k++) frame(ticks, 0.016f); }

static int failures;
#define CHECK(cond, ...) do{ if(!(cond)){ failures++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } }while(0)
#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

/* a 90 deg/s left turn for 1 s at sensitivity 1.5 turns the camera 135 deg (UE yaw grows to the right) */
static const double TURN = -135.0;

static void closed_then_open_without_jump(const char* name, void (*close)(void), void (*open)(void)){
  reset(); G.log = 1; close();
  rate[0][0] = 90; rate[0][1] = 0; rate[1][0] = rate[1][1] = 0;
  frames(70, 1);   /* 1.12 s: gyro_log reports the dropped turn once a second */
  CHECK(cam_yaw(0) == 0 && cam_pitch(0) == 0 && add_calls == 0, "%s: closed gate turned the camera %.3f / %.3f (%d calls)", name, cam_yaw(0), cam_pitch(0), add_calls);
  CHECK(orig_calls == 70, "%s: the game's UpdateRotation must always run (%d)", name, orig_calls);
  CHECK(logged("closed ("), "%s: gyro_log must report the closed gate", name);
  CHECK(logged("dropped "), "%s: gyro_log must report the dropped turn", name);
  open(); rate[0][0] = 0;
  frames(10, 1);
  CHECK(NEAR(cam_yaw(0), 0, 1e-9), "%s: opening the gate turned the camera %.4f deg (a jump from the closed time)", name, cam_yaw(0));
  rate[0][0] = 90; frames(62, 1);   /* then it works: 62 frames = 0.992 s, the first sample of a frame window integrates from the last */
  CHECK(NEAR(cam_yaw(0), TURN * 0.992, 0.02 * 135), "%s: open gate turned %.3f deg, want about %.3f", name, cam_yaw(0), TURN * 0.992);
}

/* the title: a controller without a pawn, viewing a camera actor */
static void title_close(void){ gw64(pcs[0] + PC_PAWN, 0); gw64(pcs[0] + PC_ACK_PAWN, 0); gw64(pcm_of(0) + PCM_VIEW_TARGET, 0x7000); }
static void title_open(void){ make_pc(0, 0); }
static void pending_close(void){ gw64(pcm_of(0) + PCM_PENDING_VIEW_TARGET, 0x7000); }
static void pending_open(void){ gw64(pcm_of(0) + PCM_PENDING_VIEW_TARGET, pawn_of(0)); }   /* blending to its own pawn is fine */
static void ignore_close(void){ *GP(pcs[0] + PC_IGNORE_LOOK) = 1; }
static void ignore_open(void){ *GP(pcs[0] + PC_IGNORE_LOOK) = 0; }
static void unreadable_close(void){ bad_lo = pcm_of(0) + PCM_PENDING_VIEW_TARGET; bad_hi = bad_lo + 8; }
static void unreadable_open(void){ bad_lo = bad_hi = 0; }
static void unreadable_pc_close(void){ bad_lo = pcs[0] + PC_ACK_PAWN; bad_hi = bad_lo + 8; }
static void tagged_close(void){ u64 t = pawn_of(0) | 1; gw64(pcs[0] + PC_PAWN, t); gw64(pcs[0] + PC_ACK_PAWN, t); gw64(pcm_of(0) + PCM_VIEW_TARGET, t); }
static void tagged_open(void){ make_pc(0, 0); }
static void unacked_close(void){ gw64(pcs[0] + PC_ACK_PAWN, 0); }
static void spectating_close(void){ gw64(pcm_of(0) + PCM_VIEW_TARGET, pawn_of(1)); }
static void spectating_open(void){ gw64(pcm_of(0) + PCM_VIEW_TARGET, pawn_of(0)); }
static void no_camera_close(void){ gw64(pcs[0] + PC_CAMERA_MANAGER, 0); }

static void test_both_players_turn_their_own_camera(void){
  reset();
  rate[0][0] = 90; rate[0][1] = 0; rate[1][0] = 0; rate[1][1] = 30;
  frames(62, 3);
  CHECK(NEAR(cam_yaw(0), TURN * 0.992, 2.7) && NEAR(cam_pitch(0), 0, 1e-6), "P1 turned yaw %.3f pitch %.3f", cam_yaw(0), cam_pitch(0));
  CHECK(NEAR(cam_pitch(1), 45 * 0.992, 0.9) && NEAR(cam_yaw(1), 0, 1e-6), "P2 turned yaw %.3f pitch %.3f", cam_yaw(1), cam_pitch(1));
}

/* paused: the paused tick skips PlayerTick, so UpdateRotation is not called while the pad keeps turning */
static void test_no_jump_after_pause(void){
  reset(); G.log = 1;
  rate[0][0] = 90; rate[0][1] = 0; rate[1][0] = rate[1][1] = 0;
  frames(30, 1);
  double before = cam_yaw(0);
  frames(125, 0);   /* 2 s paused, the pad still turning */
  clock_ns += 2000000ull; tick(0, 0.016f);   /* the game resumes before the pad's next samples arrive */
  rate[0][0] = 0;
  frames(5, 1);
  CHECK(NEAR(cam_yaw(0), before, 1e-9), "after a 2 s pause the camera moved %.4f deg", cam_yaw(0) - before);
  CHECK(logged("UpdateRotation again after"), "gyro_log must report the UpdateRotation gap");
  /* a pause that does call UpdateRotation with DeltaTime 0 also drops it */
  rate[0][0] = 90; before = cam_yaw(0);
  for(int k = 0; k < 60; k++) frame(1, 0.0f);
  rate[0][0] = 0; frames(5, 1);
  CHECK(NEAR(cam_yaw(0), before, 1e-9), "after DeltaTime 0 frames the camera moved %.4f deg", cam_yaw(0) - before);
}

/* synthetic test: starts only after 3 s of an open gate without a break, and passes */
static void test_synthetic_waits_for_continuous_gate(void){
  reset(); G.mode = 0; T.state = 1;
  frames(150, 1);                 /* 2.4 s open */
  ignore_close(); frames(30, 1);  /* closed 0.48 s: the 3 s start over */
  ignore_open(); frames(150, 1);  /* 2.4 s open again: not yet */
  CHECK(T.state == 1, "test started after %s", "a broken 3 s window");
  frames(50, 1);                  /* past 3 s */
  CHECK(T.state == 2 && T.pc == pcs[0], "test did not start after 3 s of open gate (state %d)", T.state);
  frames(200, 1);
  CHECK(T.state == 4 && logged("-> PASS") && !logged("-> FAIL") && !logged("INCONCLUSIVE"), "synthetic test did not pass:\n%s", logbuf);
  CHECK(NEAR(T.want_yaw, -135, 1.35) && NEAR(T.want_pitch, 45, 0.45), "synthetic want %.3f / %.3f", T.want_yaw, T.want_pitch);
}

/* the gate closes in the middle of the synthetic test: INCONCLUSIVE, never PASS */
static void test_synthetic_gate_closes_is_inconclusive(void){
  reset(); G.mode = 0; T.state = 1;
  frames(200, 1);
  CHECK(T.state == 2, "test did not start (state %d)", T.state);
  frames(20, 1);
  title_close(); frames(5, 1);
  CHECK(T.state == 4 && logged("INCONCLUSIVE") && logged("no pawn") && !logged("PASS"), "closing the gate mid-test:\n%s", logbuf);
  title_open(); frames(200, 1);
  CHECK(!logged("PASS") && !logged("FAIL"), "a result after an inconclusive end:\n%s", logbuf);
}

/* an engine that does not apply the input: FAIL */
static void test_synthetic_engine_drop_fails(void){
  reset(); G.mode = 0; T.state = 1; engine_drops = 1;
  frames(400, 1);
  CHECK(T.state == 4 && logged("-> FAIL") && !logged("-> PASS"), "a camera that did not turn must fail:\n%s", logbuf);
}

/* review cases: the 3 s window is broken by an UpdateRotation gap and by a new controller, and a test that runs is ended for good
   by a gap or by the game thread while the frontend thread is in a synthetic sample */
static void test_wait_window_broken_by_pause(void){
  reset(); G.mode = 0; T.state = 1;
  frames(150, 1);   /* 2.4 s open */
  frames(200, 0);   /* 3.2 s paused: no UpdateRotation */
  tick(0, 0.016f);
  CHECK(T.state == 1, "2.4 s open + 3.2 s without UpdateRotation started the test (state %d)", T.state);
  frames(150, 1);
  CHECK(T.state == 1, "the window counted time from before the pause (state %d)", T.state);
  frames(50, 1);
  CHECK(T.state == 2, "the test did not start after 3 s unbroken after the pause (state %d)", T.state);
  frames(200, 1);
  CHECK(T.state == 4 && logged("-> PASS") && !logged("INCONCLUSIVE"), "after a pause before the start the test must still pass:\n%s", logbuf);
}
static void test_wait_window_broken_by_short_gap(void){   /* a 128 ms gap is a break too */
  reset(); G.mode = 0; T.state = 1;
  frames(150, 1); frames(8, 0); frames(40, 1);
  CHECK(T.state == 1, "a 128 ms UpdateRotation gap did not restart the 3 s (state %d)", T.state);
}
static void test_wait_window_broken_by_controller_change(void){
  reset(); G.mode = 0; T.state = 1;
  frames(150, 1);   /* 2.4 s on the old controller */
  make_pc(2, 0);    /* a new controller with ControllerId 0, the old one stops */
  frames(50, 4);    /* 0.8 s on the new one */
  CHECK(T.state == 1, "the 3 s window carried over to a new controller (state %d)", T.state);
  frames(150, 4);
  CHECK(T.state == 2 && T.pc == pcs[2], "the new controller did not start after its own 3 s (state %d)", T.state);
}
static void test_wait_window_per_player(void){   /* P2 closed does not break P1's window */
  reset(); G.mode = 0; T.state = 1;
  *GP(pcs[1] + PC_IGNORE_LOOK) = 1;
  frames(200, 3);
  CHECK(T.state == 2 && T.slot == 0 && T.pc == pcs[0], "P1 did not start beside a closed P2 (state %d slot %d)", T.state, T.slot);
}
static void ended_for_good(const char* name){
  frames(300, 1);
  CHECK(T.state == 4 && logged("INCONCLUSIVE") && !logged("PASS") && !logged("FAIL"), "%s: must end INCONCLUSIVE for good:\n%s", name, logbuf);
}
static void test_pause_while_running_is_inconclusive(void){
  reset(); G.mode = 0; T.state = 1;
  frames(220, 1);
  CHECK(T.state == 2, "not running (state %d)", T.state);
  frames(20, 0); tick(0, 0.016f);
  CHECK(T.state == 4 && logged("no UpdateRotation for"), "a pause while running:\n%s", logbuf);
  ended_for_good("pause while running");
}
static void test_pause_while_settling_is_inconclusive(void){
  reset(); G.mode = 0; T.state = 1;
  for(int k = 0; k < 400 && T.state != 3; k++) frame(1, 0.016f);
  CHECK(T.state == 3, "not settling (state %d)", T.state);
  frames(200, 0); tick(0, 0.016f);
  CHECK(T.state == 4 && logged("INCONCLUSIVE") && !logged("PASS"), "a pause while settling:\n%s", logbuf);
  ended_for_good("pause while settling");
}
static void test_end_during_synthetic_sample_stays(void){
  reset(); G.mode = 0; T.state = 1;
  frames(200, 1);
  CHECK(T.state == 2, "not running (state %d)", T.state);
  while(T.state == 2 && T.next_sample - T.t0 < 1950000000ull) frame(1, 0.016f);
  CHECK(T.state == 2, "ended early (state %d)", T.state);
  clock_ns += 50000000ull;   /* this call's samples cross 2 s: left alone the loop would go on to settling (state 3) */
  /* the frontend reads the clock once, then once per synthetic sample: 2 = inside the first sample, with test_lock let go */
  abort_countdown = 2;
  orbitals_gyro_pad(0, 100, 0x054c, 0x0ce6, 0, 0, NULL, 0, 1, 1);
  CHECK(abort_countdown == 0, "the simulated end did not land inside a synthetic sample (%d left)", abort_countdown);
  CHECK(T.state == 4, "the frontend thread overwrote INCONCLUSIVE with state %d:\n%s", T.state, logbuf);
  double before = cam_yaw(0); int calls = add_calls;
  clock_ns += 16000000ull; tick(0, 0.016f);
  CHECK(add_calls == calls && cam_yaw(0) == before, "the ended test's synthetic turn reached the camera (%.4f deg)", cam_yaw(0) - before);
  ended_for_good("end during a synthetic sample");
}
static void test_end_inside_sample_late_in_loop(void){   /* several samples due: the end lands in a later one, the loop stops there */
  reset(); G.mode = 0; T.state = 1;
  frames(200, 1);
  CHECK(T.state == 2, "not running (state %d)", T.state);
  clock_ns += 40000000ull;   /* about 10 samples due in one call */
  double want = T.want_yaw;
  abort_countdown = 4;       /* the third sample */
  orbitals_gyro_pad(0, 100, 0x054c, 0x0ce6, 0, 0, NULL, 0, 1, 1);
  CHECK(T.state == 4 && abort_countdown == 0, "state %d countdown %d", T.state, abort_countdown);
  CHECK(fabs(T.want_yaw - want) < 3.5 * 0.004 * 90 * 1.5, "samples kept coming after the end: %.4f deg", T.want_yaw - want);
  ended_for_good("end in a later synthetic sample");
}
static void test_controller_change_drops_synthetic_turn(void){   /* the ended test's waiting turn never turns the new controller */
  reset(); G.mode = 0; T.state = 1;
  frames(220, 1);
  CHECK(T.state == 2, "not running (state %d)", T.state);
  make_pc(2, 0);
  frame(4, 0.016f);
  CHECK(T.state == 4 && logged("another player controller"), "a new controller while running:\n%s", logbuf);
  CHECK(cam_yaw(2) == 0, "the synthetic turn reached the new controller: %.4f deg", cam_yaw(2));
  ended_for_good("controller change while running");
}
static void test_end_inside_last_due_sample(void){   /* the end lands in the last due sample: the loop exits without a state check */
  reset(); G.mode = 0; T.state = 1;
  frames(200, 1);
  CHECK(T.state == 2, "not running (state %d)", T.state);
  clock_ns = T.next_sample;  /* exactly one sample due */
  double before = cam_yaw(0); int calls = add_calls;
  abort_countdown = 2;       /* inside that sample, after the game thread could have ended the test and drained Acc */
  orbitals_gyro_pad(0, 100, 0x054c, 0x0ce6, 0, 0, NULL, 0, 1, 1);
  CHECK(abort_countdown == 0 && T.state == 4, "state %d countdown %d", T.state, abort_countdown);
  CHECK(Acc[0].yaw == 0 && Acc[0].pitch == 0, "the last sample published after the end: yaw %.6f pitch %.6f", Acc[0].yaw, Acc[0].pitch);
  clock_ns += 16000000ull; tick(0, 0.016f);
  CHECK(add_calls == calls && cam_yaw(0) == before, "camera turned %.6f deg after the end (%d AddInput calls)", cam_yaw(0) - before, add_calls - calls);
  ended_for_good("end inside the last due sample");
}
static void test_end_after_publication_clears_it(void){   /* the turn published before a post-UpdateRotation end is dropped */
  reset(); G.mode = 0; T.state = 1;
  frames(200, 1);
  CHECK(T.state == 2, "not running (state %d)", T.state);
  clock_ns += 8000000ull; orbitals_gyro_pad(0, 100, 0x054c, 0x0ce6, 0, 0, NULL, 0, 1, 1);
  CHECK(Acc[0].yaw != 0, "no synthetic turn waiting");
  test_lock(); test_inconclusive("simulated end after UpdateRotation"); test_unlock();
  CHECK(Acc[0].yaw == 0 && Acc[0].pitch == 0, "the waiting synthetic turn survived the end: %.6f", Acc[0].yaw);
  double before = cam_yaw(0); clock_ns += 16000000ull; tick(0, 0.016f);
  CHECK(cam_yaw(0) == before, "camera turned %.6f deg after the end", cam_yaw(0) - before);
  ended_for_good("end after a publication");
}
static void test_end_matrix(void){   /* running and settling x closed gate, gap, new controller: INCONCLUSIVE for good */
  for(int state = 2; state <= 3; state++) for(int kind = 0; kind < 3; kind++){
    reset(); G.mode = 0; T.state = 1;
    for(int k = 0; k < 400 && T.state != state; k++) frame(1, 0.016f);
    CHECK(T.state == state, "matrix preparation %d", state);
    if(kind == 0){ ignore_close(); tick(0, 0.016f); ignore_open(); }
    if(kind == 1){ clock_ns += 100000001ull; tick(0, 0.016f); }
    if(kind == 2){ make_pc(2, 0); tick(2, 0.016f); }
    CHECK(T.state == 4 && logged("INCONCLUSIVE"), "matrix end state %d kind %d", state, kind);
    ended_for_good("matrix");
  }
}

/* The frontend can enter settling after the game thread captured its clock but before it acquires test_lock. */
static void test_settle_after_clock_snapshot(void){
  reset(); G.mode=0; T.state=1; frames(200,1);
  while(clock_ns<T.t0+1980000000ull) frame(1,0.016f);
  clock_ns=T.t0+2000000000ull-1;
  settle_countdown=2; tick(0,0.016f);
  CHECK(T.state==3 && !logged("yaw integral"), "settling finished with a future start: state=%d pending_pitch=%.6f", T.state, Acc[0].pitch);
  frames(30,1);
  CHECK(T.state==4 && logged("-> PASS"), "settling did not finish after consuming pending motion:\n%s", logbuf);
}

int main(void){
  g_mem = calloc(1, MEM);
  static GuestFn disp[64]; g_disp = disp;
  g_add_yaw = g_code_lo; g_add_pitch = g_code_lo + 4;
  disp[0] = fake_add_yaw; disp[1] = fake_add_pitch;
  closed_then_open_without_jump("title (no pawn)", title_close, title_open);
  closed_then_open_without_jump("pending view target another actor", pending_close, pending_open);
  closed_then_open_without_jump("ignore look input", ignore_close, ignore_open);
  closed_then_open_without_jump("camera manager unreadable", unreadable_close, unreadable_open);
  closed_then_open_without_jump("controller field unreadable", unreadable_pc_close, unreadable_open);
  closed_then_open_without_jump("tagged pawn pointer", tagged_close, tagged_open);
  closed_then_open_without_jump("pawn not acknowledged", unacked_close, title_open);
  closed_then_open_without_jump("viewing another pawn", spectating_close, spectating_open);
  closed_then_open_without_jump("no camera manager", no_camera_close, title_open);
  test_both_players_turn_their_own_camera();
  test_no_jump_after_pause();
  test_synthetic_waits_for_continuous_gate();
  test_synthetic_gate_closes_is_inconclusive();
  test_synthetic_engine_drop_fails();
  test_wait_window_broken_by_pause();
  test_wait_window_broken_by_short_gap();
  test_wait_window_broken_by_controller_change();
  test_wait_window_per_player();
  test_pause_while_running_is_inconclusive();
  test_pause_while_settling_is_inconclusive();
  test_end_during_synthetic_sample_stays();
  test_end_inside_sample_late_in_loop();
  test_controller_change_drops_synthetic_turn();
  test_end_matrix();
  test_end_inside_last_due_sample();
  test_end_after_publication_clears_it();
  test_settle_after_clock_snapshot();
  printf(failures ? "%d FAILED\n" : "all gyro gate tests passed\n", failures);
  return failures != 0;
}
