# Scripted input (driving the game without a person)

Source of truth: `runtime/backend_win32.c` (`scripted_buttons`, `script_file_poll`, `video_poll`). Read this before writing a
scene script in `tools/dev4k/scenes/`.

## Where scripts come from
- `script=` / `script2=` in Orbitals.ini (or `--script=` on the command line): one script for player 1 / player 2, timed from
  the start of the game.
- `script_file=<path>` in Orbitals.ini: a file the runtime re-reads whenever its modification time changes (checked every
  100 ms). It holds three lines:
  ```
  <step name>
  <player 1 script>
  <player 2 script>
  ```
  Each new version of the file is a new *step*: both scripts are timed from the moment the step is read. A file with fewer
  than three lines is treated as half written and read again at the next check. `tools/dev4k/orb.sh press "<p1>" ["<p2>"]`
  writes this file on the gaming PC (`C:\llm\tmp\orb\input.txt`, step name `step`).
- With `script_file` (or a non-empty script) set, both controllers count as connected even without physical pads, and
  the script's buttons are ORed with the real pads' (a physical pad that is touched still acts).

## Script syntax
A comma-separated list of presses: `<ms>:<hex mask>[/<duration ms>]`, e.g. `0:1,1500:1,3000:2/100`.
- `ms`: when the press starts, on the step's clock.
- `mask`: hexadecimal bits, held together:

| bit | button | bit | button |
|---|---|---|---|
| 0 | A (right face button) | 8 | ZL |
| 1 | B (bottom) | 9 | ZR |
| 2 | X (top) | 10 | Plus |
| 3 | Y (left) | 11 | Minus |
| 4 | left stick click | 12 | D-pad left |
| 5 | right stick click | 13 | D-pad up |
| 6 | L | 14 | D-pad right |
| 7 | R | 15 | D-pad down |

  Analog sticks, full deflection: bit 40 left stick left, 42 left stick right, 41 left stick up, 43 left stick down;
  44 right stick left, 46 right stick right, 45 right stick up, 47 right stick down (e.g. `0:20000000000/3000` = hold the
  left stick up for 3 s: bit 41 = 0x20000000000). Bits 16-23 (stick directions as buttons) are also accepted.
- `duration`: how long it is held; default 300 ms, or 1400 ms when the mask has a stick bit (>= 40).
- Parsing stops at the first malformed entry.

## Repeatable co-op scene

Use `tools/dev4k/scenes/coop_ship.txt` with Nintendo layout and `tools/dev4k/scenes/coop_ship_bottom.txt`
with Xbox or PlayStation (bottom accept). The second script preserves every time, duration and stick action and
changes only face-button mask1 to2. It is the script used for the observed PlayStation/Xbox checks and layout/flight
benchmarks. Use the fixed copied benchmark profile; progress to other areas only in a separate validation copy.

At nightly close, `script_file` is removed from the installed baseINI (it was absent from the original), and the input file
is neutralized. This prevents script mode from making two controllers appear connected during normal play.
For a repeat bench on the copied test profile, pass `script_file=C:\llm\tmp\orb\input.txt` as an extraINI argument to
`bench.sh`; its temporary marker block is removed after the run. Manual `orb.sh press` requires that key in the running
session. Do not enable it in the owner's profile just to reproduce a benchmark.

## The script clock
By default real time minus the time the renderer spent waiting for shaders to be translated, so a held stick moves a
character as far in a first session as in later ones. `SWR_INPUT_CLOCK=game`: the clock moves only when the game presents a
frame (each present counts at most 100 ms), for replaying a route recorded by hand through freezes.

## Recording and replaying a route
- `SWR_INPUT_RECORD=<file>` appends every change of either player's final pad state as `ms player buttons lx ly rx ry`
  (`# step n at ms` lines mark script-file steps).
- A script-file step whose player 1 line is `@<file>` replays such a recording from that step, analog axes included, in place of
  the pads; the next step ends the replay.

## The log
Each step logs `input script file: step N '<name>' from <ms> ms (...)`, and each change of the scripted buttons logs
`input test: <ms> ms P1 <mask> x<lx> P2 <mask> x<lx>`. `tools/dev4k/log.sh '<regex>'` prints matching lines of the newest
session log on the PC.
