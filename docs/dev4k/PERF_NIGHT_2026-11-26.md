# Co-op performance on the owner's PC (Ryzen 7 9800X3D, RTX 5090, 4K 240 Hz) — night of 2026-11-26

Bench: `tools/dev4k/bench.sh` + scene `tools/dev4k/scenes/coop_ship.txt` (owner's save copy in the launcher test profile,
co-op split screen in the asteroid field, both players idle), window 120-180 s, `ue_fps=60`, `env=NXRT_VK_DEFER=1` (as
installed), launcher env as PLAY sets it (`NXRT_VK_WB_GPU=1`). Stats: `tools/dev4k/bench_stats.py` (late = interval > 17.7 ms).

| Variant | runs | FPS | p50 | p95 | p99 | late % | > 30 ms % | transl. CPU ms | GPU ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 2160p base | 5 | 42.17-42.56 | 22.5-22.8 | 32.9-34.6 | 37.8-38.7 | 98.9-99.5 | 7.7-8.7 | 14.1-14.5 | 19.6-19.7 |
| 2160p `NXRT_VK_SPLIT_MS=2` | 2 | 44.11 / 43.96 | 22.1 / 22.2 | 27.4 / 27.5 | 31.8 / 31.7 | 99.5 | 1.74 / 1.71 | 14.0 / 13.8 | 19.6 |
| 2160p `NXRT_VK_SPLIT_MS=1` | 1 | 43.65 | 22.2 | 29.4 | 34.4 | 98.6 | 4.28 | 14.8 | 19.9 |
| 2160p `NXRT_VK_WB_GPU=0` | 2 | 42.65 / 42.34 | 22.3 / 22.4 | 30.0 / 30.3 | 31.5 / 31.7 | 100 | 4.8 / 6.2 | 15.5 | 16.9 |
| 2160p split 2 + WB_GPU 0 + staging 512 | 2 | 42.52 / 42.05 | 22.5 / 22.8 | 29.7 / 30.0 | 31.3 / 31.9 | 100 | 3.7 / 4.9 | 15.9 / 16.0 | 16.8 / 16.9 |
| 2160p `NXRT_VK_STAGING_MB=512` | 1 | 43.36 | 22.2 | 31.4 | 38.1 | 99.5 | 5.65 | 14.2 | 19.6 |
| 2160p `NXRT_VK_DEFER=0` | 1 | 41.62 | 23.1 | 32.1 | 35.3 | 99.7 | 8.2 | 16.7 | 19.7 |
| 2160p `NXRT_VK_PASS_BARRIER=0` (wrong picture) | 1 | 44.24 | 21.6 | 31.4 | 36.1 | 97.1 | 6.1 | 14.7 | 18.0 |
| 2160p `NXRT_VK_WB_OFF=1` (test only) | 1 | game stops (svcBreak at 75 s): the CPU reads written-back data | | | | | | | |
| 2160p `NXRT_VK_SPLIT_MS=0` | 1 | never reached the scene (rejected) | | | | | | | |
| 2160p output, 3D 1800p (`scene_resolution=900`) | 1 | 47.12 | 20.5 | 26.9 | 30.1 | 95.8 | 1.2 | 12.5 | 17.6 |
| 2160p output, 3D 1440p (`scene_resolution=720`) | 1 | 52.30 | 18.5 | 23.5 | 25.2 | 66.2 | 0.03 | 9.7 | 15.9 |
| 1800p base | 1 | 47.17 | 20.6 | 26.6 | 30.0 | 95.3 | 0.99 | 12.4 | 17.5 |
| 1620p base | 1 | 51.16 | 18.9 | 24.1 | 26.5 | 74.8 | 0.16 | 10.3 | 16.5 |
| 1620p `NXRT_VK_SPLIT_MS=2` | 1 | 51.85 | 18.6 | 23.9 | 25.5 | 69.5 | 0.10 | 10.4 | 16.5 |
| 1080p base | 1 | 54.55 | 17.7 | 22.7 | 23.9 | 49.9 | 0 | 8.6 | 13.9 |

Selected for final validation: `NXRT_VK_SPLIT_MS=2` (outside the base's spread on FPS, p95, p99 and spikes; a submission split every 2 ms of
recording instead of 4 lets the GPU start sooner; same picture: screenshots at the same scene moment differ only by
animation). On the laptop 2 ms had been measured worse at p95 (vk_replay.c comment): it is a per-PC setting, applied through
`env=` in the game's Orbitals.ini. At resume the kit ini did not yet retain this setting; the measurements used temporary
bench overrides. DEFER=1 confirmed (2.3 ms less translation CPU than DEFER=0).

## Why 4K 60 is not reached (evidence)
- Even 1080p output does not hold 60 (54.6 FPS, half the frames late): a fixed cost per frame in the NVN -> Vulkan replay.
- The game draws its 3D at the full output size (passes of 3840x2160; the Switch 2 renders 1080p) and the replay's cost grows
  with it on both sides: translation CPU 8.6 ms (1080p) -> 14.4 ms (2160p), GPU 13.9 -> 19.7 ms.
- Per frame at 2160p: 150-210 MB of logical writeback ranges, not compact returned bytes. The 120-180 s base-a samples average
  165.54 MiB of logical ranges and about 4.38 MiB of compact records, plus snapshot and fallback traffic. The full comparison
  baseline still travels through host memory. Uploads average 52.84 MiB (4.44 ms CPU). The staging ring is **256 MB**;
  64 MB is the upload budget, not ring capacity. The ring fills (~35 waits a second, 2 at 1080p).
- GPU timestamp intervals (`NXRT_VK_GPUTIME_AT_S`): 7.54 ms assigned to submission starts and 4.84 ms assigned to 42 small
  dispatches include uploads, previous writebacks, preparation and barriers. They do not establish pure idle time or shader
  execution time. Game threads wait ~18 ms a frame in nvnSyncWait; this includes completion effects and is not by itself proof
  of an avoidable serial stall.

## Resume: icon scanner regression
With the final memory-lock safety patch installed, the old PlayStation scanner severely harms frame pacing. At 1800p,
same scene/window: PlayStation 36.03/34.37 FPS, p95 64.57/64.55 ms, p99 138.85/163.46 ms; Nintendo 48.30 FPS,
p95 25.37 ms, p99 29.53 ms. The second PlayStation run had no OpenRGB detection, ruling out that initial explanation.
The direct icon-lookup replacement must be verified before keeping PlayStation as the owner's default.

## Next work by expected gain (not done tonight)
1. Compare snapshots in device-local memory while retaining compact host output and CPU/GPU coherence. First implementation
   failed review for lifetime and allocation-failure handling; do not install it until corrected and differentially checked.
   Readbacks on demand are a larger alternative requiring read-fault tracking; current dirty bits detect writes only.
2. Dependency-exact barriers (ceiling measured: PASS_BARRIER=0 gave +2 FPS / -1.7 ms GPU) and batching the 42 tiny dispatches.
3. A larger staging ring sized from the frame's uploads (512 MB helped +1.1 FPS but waits remained).
4. 3D resolution below the output at 4K (`scene_resolution`) is the only setting that approaches 60 today (1440p 3D: 52 FPS);
   it is a picture change, so it stays the owner's choice.
5. 90 / 120 / unlimited: not tried; nothing reaches 60 yet, and the runtime's frame pacing emulates a 60 Hz display.
