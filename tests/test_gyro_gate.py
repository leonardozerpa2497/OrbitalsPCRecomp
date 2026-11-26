"""Build and run the gyro gate tests (tests/test_gyro_gate.c) against the runtime's real orbitals_input.c, then check that each
mutation of a key gate rule (controller type, controlled actor, owner, cinematic, shared pause, class metadata checks, synthetic
test start delay) fails them."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "runtime" / "orbitals_input.c"

CLASS_CHECK = "  if(!gyro_isa(pc,CLS_PLAYER_CONTROLLER)) return GATE_NOT_GAME_CONTROLLER;   /* before any game field */\n"
EXTENDED_READ = "  q->cinematic=*GP(pc+PC_CINEMATIC); q->main=gr64(pc+PC_MAIN_CHARACTER); q->controlled=gr64(pc+PC_CONTROLLED_ACTOR);\n"
MUTATIONS = {
    "latest expiry removed": [("now-Acc[id].rate_ns<=50000000ull", "1")],
    "latest tick cap removed": [("fmin((now-taken)/1e9,0.05)", "((now-taken)/1e9)")],
    "latest sensitivity ignored": [("Acc[s].rate_yaw=dyaw/dt", "Acc[s].rate_yaw=dyaw/dt/G.sens_x")],
    "latest inversion ignored": [("if(G.invert&1)", "if(0)")],
    "latest parser ignored": [("G.latest=!strcmp(v,\"latest\")", "G.latest=0")],
    "latest real producer during test": [("if(G.latest && !G.test){", "if(G.latest){")],
    "latest consumer during test": [("if(G.latest && !G.test && open", "if(G.latest && open")],
    "no controller class check": [(CLASS_CHECK, "")],
    "controller class checked after the game fields": [(CLASS_CHECK, ""), (EXTENDED_READ, EXTENDED_READ + CLASS_CHECK)],
    "chain entry compared with the class": [("  return entry==want+CLASS_BASE_CHAIN;", "  return entry==want;")],
    "no wanted depth above actual depth check": [("  if(wd>d) return 0;", "")],
    # a negative depth also makes every chain offset >= 2^32, which gyro_field64 refuses: removing the sign check alone is equivalent
    "no negative depth check nor offset bound": [("*depth>=0 && ", ""), (" || off>=(1ull<<32)", "")],
    "no depth cap": [(" && *depth<=GYRO_MAX_DEPTH", "")],
    "no wanted class self check": [("  if(!gyro_field64(wchain,(u64)wd*8,&entry) || entry!=want+CLASS_BASE_CHAIN) return -1;\n", "")],
    "invalid Outer walked past as not a level": [("    if(is<0) return -1;", "")],
    "no own-depth self check of the object's class": [("  if(!gyro_field64(chain,(u64)d*8,&entry) || entry!=cls+CLASS_BASE_CHAIN) return -1;\n", "")],
    "field base not checked": [("  if(!gyro_ptr(base) || off>=(1ull<<32) ||", "  if(off>=(1ull<<32) ||")],
    "view target compared with the pawn": [("  if(q->vt!=actor) return GATE_VIEW_TARGET;", "  if(q->vt!=q->pawn) return GATE_VIEW_TARGET;")],
    "no MainCharacter fallback": [("u64 actor= q->controlled? q->controlled : q->main;", "u64 actor= q->controlled;")],
    "pending compared with the pawn": [("q->pending!=actor", "q->pending!=q->pawn")],
    "no controllable actor type check": [("    if(!gyro_isa(actor,CLS_CONTROLLABLE_ACTOR)) return GATE_CONTROLLED_ACTOR;   /* before its controller field */\n", "")],
    "no owner check": [("    if(q->owner!=pc) return GATE_WRONG_OWNER;\n", "")],
    "no main character check": [("  if(!gyro_ptr(q->main)) return GATE_NO_MAIN_CHARACTER;\n", "")],
    "no cinematic check": [("  if(q->cinematic&PC_CINEMATIC_BIT) return GATE_CINEMATIC;\n", "")],
    "no shared pause check": [("  if(q->paused) return GATE_PAUSED;\n", "")],
    "game state not found accepted": [("  if(q->paused<0) return GATE_NO_GAME_STATE;\n", "")],
    "no game state type check": [("  if(!gyro_isa(gs,CLS_GAME_STATE) || !guest_span_readable(gs+GS_PAUSED,1,1)) return -1;",
                                  "  if(!guest_span_readable(gs+GS_PAUSED,1,1)) return -1;")],
    "no Outer bound": [("for(int n=0;n<GYRO_MAX_OUTERS && !level;n++){", "for(int n=0;!level;n++){ if(n>=GYRO_MAX_OUTERS) n=0;")],
    "first Outer taken as the level": [("    if(is) level=o;", "    if(1) level=o;")],
    "no gyro_test_delay check": [(" && now>=T.armed_ns+T.delay_ns", "")],
    "gyro_test_delay counted from the clock origin": [("T.armed_ns=now;", "T.armed_ns=0;")],
    "gyro_test_delay setting ignored": [("T.delay_ns=(u64)(G.test_delay*1e9);", "T.delay_ns=0;")],
    "gyro_test_delay not bounded": [("if(f>=0 && f<=600) G.test_delay=f;", "G.test_delay=f;")],
    "no 3 s unbroken gate before the start": [("Gs[id].open_since && now-Gs[id].open_since>3000000000ull", "Gs[id].open_since")],
}


def build_and_run(runtime_dir, build):
    exe = Path(build) / "gyro_gate"
    result = subprocess.run([os.environ.get("CC", "gcc"), "-std=gnu11", "-ffunction-sections", "-fdata-sections",
                             "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-I", str(runtime_dir), "-I", str(ROOT / "runtime"),
                             str(ROOT / "tests" / "test_gyro_gate.c"), "-Wl,--gc-sections", "-lm", "-o", str(exe)],
                            capture_output=True, text=True)
    if result.returncode:
        return None, result.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    return run.returncode, run.stdout + run.stderr


class GyroGateTests(unittest.TestCase):
    def test_gate(self):
        with tempfile.TemporaryDirectory(prefix="orbitals-gyro-gate-") as build:
            code, out = build_and_run(ROOT / "runtime", build)
            self.assertEqual(code, 0, out)

    def test_mutations_fail(self):
        source = RUNTIME.read_text()
        for name, edits in MUTATIONS.items():
            with self.subTest(mutation=name), tempfile.TemporaryDirectory(prefix="orbitals-gyro-mut-") as build:
                mutated = source
                for old, new in edits:
                    self.assertIn(old, mutated, "mutation no longer applies; update it with the gate")
                    mutated = mutated.replace(old, new, 1)
                (Path(build) / "orbitals_input.c").write_text(mutated)
                shutil.copy(ROOT / "runtime" / "orbitals_input.h", build)
                code, out = build_and_run(build, build)
                self.assertIsNotNone(code, out)
                self.assertNotEqual(code, 0, f"mutation '{name}' passed every test")


if __name__ == "__main__":
    unittest.main()
