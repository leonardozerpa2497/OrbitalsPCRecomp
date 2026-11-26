# Selected-layout performance gate

The installed scanner-free icon hooks and bottom-accept redirects show no consistent FPS regression in this co-op scene. This is a feature-cost check, not evidence of stable 60 FPS or an optimization gain.

Same reviewed runtime and regenerated game module throughout; copied test profile; gyro and icon trace off; DEFER=1, SPLIT_MS=2, WB_GPU=1, WB_BASE_DEVICE=1. Nintendo uses the original scene script; PlayStation uses the equivalent bottom-accept navigation script. Each run lasts 185 seconds, analyzed over 120–180 seconds, with one screenshot at 150 seconds. Order N/P/N/P at each resolution.

| Run | FPS | p50 ms | p95 ms | p99 ms | >17.7 ms | >30 ms | Translation CPU ms | GPU ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Nintendo 1800 a | 58.10 | 16.67 | 25.37 | 28.42 | 22.84% | 0.32% | 10.41 | 11.79 |
| PlayStation 1800 a | 58.17 | 16.79 | 20.14 | 26.69 | 18.77% | 0.43% | 10.49 | 11.80 |
| Nintendo 1800 b | 58.16 | 16.67 | 20.13 | 25.93 | 16.57% | 0.43% | 10.54 | 11.75 |
| PlayStation 1800 b | 58.25 | 16.81 | 20.14 | 23.44 | 19.20% | 0.37% | 10.62 | 11.80 |
| Nintendo 2160 a | 52.17 | 18.38 | 26.39 | 30.72 | 59.33% | 1.41% | 14.67 | 13.33 |
| PlayStation 2160 a | 52.32 | 18.30 | 26.27 | 31.08 | 59.18% | 1.43% | 14.71 | 13.33 |
| Nintendo 2160 b | 52.28 | 18.06 | 27.32 | 32.16 | 55.55% | 2.68% | 14.64 | 13.28 |
| PlayStation 2160 b | 51.91 | 18.31 | 28.06 | 32.43 | 58.22% | 2.86% | 14.56 | 13.33 |

At 1800p the average FPS difference is approximately +0.14%; at 2160p approximately −0.21%. The two 4K pairs change sign, so neither a consistent loss nor a gain is established. Tail variation is substantial even between Nintendo controls. This sample does not establish zero overhead in every screen.

All eight screenshots were inspected: same ship scene and camera directions, Nintendo ZR/Y versus PlayStation R2/Square, no obvious scene regression. Logs confirm the PlayStation action hook and six identity redirects; Nintendo installs neither button hook. No RED messages were found in the measured windows checked, and the device-snapshot dropped-write counter remained zero.

Evidence: `/tmp/claude-1000/bench/layout-{nintendo,playstation}-{1800,2160}{a,b}/` and matching screenshots under `/tmp/claude-1000/orb/`. Recompute using `tools/dev4k/bench_stats.py --from 120 --to 180 <run directories>`.

After this gate, launcher commit `921196b` (test cleanup `18ebfae`) was installed: Xbox/PlayStation imply bottom acceptance; Nintendo writes neither override. The real Windows Controls page was observed in `launcher-controls-visible_s.png`, with PlayStation selected, gyro Off, and both DualSense Edge endpoints detected. Backup: `orbitals-night-backup-20261126/launcher-before-layout-selection-20261126-100529` on the desktop.

Undo controls through the launcher by selecting Nintendo. To restore the previous launcher UI, copy its backed-up `core.py` and `page_settings.py` into the kit's `launcher` directory with the app closed.
