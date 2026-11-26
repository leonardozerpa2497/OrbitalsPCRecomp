# Flight recorder comparison

Eight alternating runs on the same copied test profile and ship scene, PlayStation layout/bottom accept, gyro off,
device snapshots1, SPLIT_MS2, DEFER1, WB_GPU1, staging256 MB.185 seconds per run, analysis120–180 seconds.
Only `NXRT_VK_FLIGHT` changes. All eight screenshots were inspected: same scene, no obvious visual regression.

| Run | FPS | p50 ms | p95 ms | p99 ms | >17.7 ms % | >30 ms % | Translation CPU ms | GPU ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| flight-1-1800a |57.55|16.67|20.84|28.26|22.36|0.72|11.06|11.94|
| flight-0-1800a |57.97|16.80|20.70|28.51|21.92|0.52|10.55|11.81|
| flight-1-1800b |57.08|16.67|21.43|29.30|23.95|0.79|11.13|11.98|
| flight-0-1800b |57.57|16.67|20.74|30.30|18.94|1.13|10.45|11.83|
| flight-1-2160a |51.30|18.51|27.64|32.18|62.01|3.05|15.10|13.50|
| flight-0-2160a |52.21|18.05|26.97|32.09|55.24|2.94|14.38|13.33|
| flight-1-2160b |51.63|18.34|28.00|32.23|59.15|2.97|14.86|13.53|
| flight-0-2160b |51.91|18.34|27.54|32.36|57.93|2.70|14.61|13.30|

Select `env=NXRT_VK_FLIGHT=0` for the gaming-PC performance configuration. Translation CPU ranges separate at both
resolutions, saving0.60 ms at1800p and0.48 ms at2160p on average. GPU saves0.15/0.19 ms. Mean FPS gains are small:
0.45/0.60 FPS. Both alternating pairs improve FPS and p95;1800p FPS ranges almost touch, so do not describe it as a large gain.
At1800p p99 and >30 ms spikes do not improve consistently. This does not meet stable60 or the <1% late-frame requirement.

The option disables diagnostic draw/capture recording, reducing information available for crash investigations. Restore
`env=NXRT_VK_FLIGHT=1` (or remove the override) when collecting that diagnostic. It does not change render resolution.
Evidence: `/tmp/claude-1000/bench/<run>/{frames.csv,Orbitals.log}` and `/tmp/claude-1000/orb/<run>_s.png`.
Recompute using `tools/dev4k/bench_stats.py --from 120 --to 180 <run>`. The reusable scene is
`tools/dev4k/scenes/coop_ship_bottom.txt`. Existing startup stub/shader warnings also occur in the control build;
they are not evidence of a new flight-option regression. Scope is this scene, not every level.
