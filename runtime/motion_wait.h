/* Keep sensor delivery independent of the 60 Hz frontend without another SDL thread.
   The deadline belongs to the frontend: housekeeping, HID and emulated vsync keep their cadence. */
#ifndef SWR_MOTION_WAIT_H
#define SWR_MOTION_WAIT_H
#include <stdint.h>
static void motion_wait_until(uint64_t deadline, uint64_t (*clock_ns)(void),
                              void (*sleep_ns)(int64_t), void (*poll_motion)(void)) {
  for (;;) {
    uint64_t now=clock_ns();
    if(now>=deadline) return;
    uint64_t remaining=deadline-now;
    if(remaining<=2000000ull) { sleep_ns((int64_t)remaining); return; }
    sleep_ns(2000000);
    if(clock_ns()>=deadline) return; /* an oversleep never creates catch-up polls */
    poll_motion();
  }
}
#endif
