# Device snapshot benchmark

The device-local snapshot option improves average 1800p and 2160p performance in two alternating runs each, but does **not** reach stable 60 FPS. Do not interpret the average improvement as a complete frame-pacing fix.

## Method

Copied launcher test profile, Nintendo layout, gyro off, `coop_ship.txt`, 185-second runs, analysis window 120–180 seconds. All runs use the same reviewed runtime with snapshot ordering and lifetime fixes, `DEFER=1`, `SPLIT_MS=2`, `WB_GPU=1`, staging 256 MB. Only `WB_BASE_DEVICE` changes. The order is A/B/A/B per resolution. No owner save is used.

| Run | Device baseline | FPS | p50 ms | p95 ms | p99 ms | >17.7 ms | >30 ms | Translation CPU ms | GPU ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| wb-held-0-2160a | 0 | 44.02 | 21.95 | 27.97 | 32.48 | 99.58% | 2.46% | 14.63 | 19.35 |
| wb-held-1-2160a | 1 | 51.77 | 18.28 | 27.65 | 32.11 | 58.07% | 2.80% | 14.76 | 13.37 |
| wb-held-0-2160b | 0 | 44.04 | 22.09 | 27.40 | 31.10 | 99.85% | 1.40% | 14.12 | 19.46 |
| wb-held-1-2160b | 1 | 52.13 | 18.21 | 27.83 | 32.19 | 56.32% | 2.97% | 14.62 | 13.29 |
| wb-held-0-1800a | 0 | 49.69 | 19.53 | 24.62 | 26.79 | 86.88% | 0.13% | 11.27 | 16.77 |
| wb-held-1-1800a | 1 | 58.12 | 16.67 | 20.09 | 25.88 | 17.04% | 0.55% | 10.39 | 11.79 |
| wb-held-0-1800b | 0 | 50.38 | 19.23 | 24.20 | 26.06 | 81.47% | 0.17% | 11.08 | 16.80 |
| wb-held-1-1800b | 1 | 58.18 | 16.67 | 20.01 | 25.06 | 15.79% | 0.43% | 10.46 | 11.77 |

At 2160p average FPS rises approximately 18%; GPU time falls approximately 31%. Translation CPU time does not improve. The p95 ranges overlap, and the share above 30 ms is higher with the option on in these samples. At 1800p average FPS rises approximately 16%, and p95 improves from 24.2–24.6 ms to 20.0–20.1 ms. The share above 30 ms remains below 1%, but increases. Further work must address the tail; the target remains less than 1% above 17.7 ms, which neither resolution meets.

Evidence: `/tmp/claude-1000/bench/<run>/{frames.csv,Orbitals.log}`. Recompute with `tools/dev4k/bench_stats.py --from 120 --to 180 <run directory>`. Corresponding screenshots are `/tmp/claude-1000/orb/<run>_s.png`; both alternating pairs show the same ship scene, with no obvious visual regression on inspection. This is a finite scene comparison, not validation of every level or shader.

Correctness oracle and forced fallback results are in [WB_BASE_DEVICE.md](WB_BASE_DEVICE.md). Option remains off by default. Revert the experiment by removing `env=NXRT_VK_WB_BASE_DEVICE=1` or setting it to zero; the snapshot ordering fix remains in both variants.
