"""Exercise the actual sensor wait scheduler with independent camera and sensor clocks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include "motion_wait.h"
static uint64_t now, last_poll, worst_gap, poll_cost, oversleep, camera, last_camera_sample;
static int polls, empty_frames, frames;
static uint64_t clock_ns(void) { return now; }
static void sleep_ns(int64_t ns) {
  assert(ns>0);
  uint64_t end=now+(uint64_t)ns+oversleep;
  while(camera && camera<=end) {
    /* Constant speed: every 2 ms sensor interval contributes the same angle.
       A frame with no newly delivered intervals visibly freezes its rotation. */
    uint64_t sample=last_poll/2000000;
    if(sample==last_camera_sample && frames>2) empty_frames++;
    last_camera_sample=sample; frames++;
    camera+=16600000; /* slightly faster independent game clock */
  }
  now=end;
}
static void poll(void) {
  if(now-last_poll>worst_gap) worst_gap=now-last_poll;
  last_poll=now; polls++; now+=poll_cost;
}
static void reset(void) {
  now=last_poll=worst_gap=poll_cost=oversleep=camera=last_camera_sample=0;
  polls=empty_frames=frames=0;
}
int main(void) {
  reset(); motion_wait_until(16666667,clock_ns,sleep_ns,poll);
  assert(now==16666667 && polls==8 && worst_gap==2000000);
  reset(); now=20000000; motion_wait_until(16666667,clock_ns,sleep_ns,poll);
  assert(polls==0 && now==20000000);
  reset(); oversleep=20000000; motion_wait_until(16666667,clock_ns,sleep_ns,poll);
  assert(polls==0); /* no backlog of catch-up polls */
  reset(); poll_cost=18000000; motion_wait_until(16666667,clock_ns,sleep_ns,poll);
  assert(polls==1); /* a slow poll gives control back to the frontend */
  int slow_empty;
  reset(); camera=1000000;
  for(int i=0;i<1200;i++) { poll(); sleep_ns(16666667); }
  slow_empty=empty_frames; assert(slow_empty>0);
  reset(); camera=1000000;
  for(int i=0;i<1200;i++) { poll(); motion_wait_until((uint64_t)(i+1)*16666667,clock_ns,sleep_ns,poll); }
  assert(empty_frames==0 && worst_gap<=2000000);
  printf("independent camera clock: old empty frames=%d, fast delivery=%d; scheduler OK\n",slow_empty,empty_frames);
}
'''


class MotionWaitTest(unittest.TestCase):
    def test_sensor_delivery_and_deadlines(self):
        cc = shutil.which("gcc") or shutil.which("clang")
        self.assertIsNotNone(cc)
        with tempfile.TemporaryDirectory(prefix="orbitals-motion-wait-") as tmp:
            source = Path(tmp) / "test.c"
            exe = Path(tmp) / "test"
            source.write_text(SOURCE)
            subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "runtime"),
                            str(source), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
