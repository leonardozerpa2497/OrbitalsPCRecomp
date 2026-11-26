# Staging ring comparison

Eight alternating185-second runs, same copied profile and ship scene, window120–180s. PlayStation/bottom accept,
gyroOff, DEFER1, SPLIT2, WB_GPU1, WB_BASE_DEVICE1, FLIGHT0 and effective sync1. Only STAGING_MB changes256/512.
Every effective sync read is1/1; startup logs confirm each staging size. All eight screenshots were inspected:
same scene and no obvious visual regression. Existing startup stub/shader warnings occur in both variants.

| Run | FPS | p50 ms | p95 ms | p99 ms | >17.7 ms % | >30 ms % | Translation CPU ms | GPU ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| staging-256-1800a |58.58|16.67|19.56|22.08|15.91|0.00|10.36|11.84|
| staging-512-1800a |58.77|16.67|19.11|21.35|14.89|0.00|10.37|11.81|
| staging-256-1800b |58.57|16.67|19.48|22.23|14.63|0.00|10.34|11.82|
| staging-512-1800b |58.65|16.67|19.45|21.35|15.86|0.00|10.39|11.73|
| staging-256-2160a |54.36|17.39|24.61|28.22|45.57|0.34|13.83|13.39|
| staging-512-2160a |55.57|17.07|23.01|26.31|40.20|0.18|13.99|13.34|
| staging-256-2160b |54.62|17.23|24.58|28.72|42.95|0.61|13.76|13.29|
| staging-512-2160b |55.04|17.36|23.26|27.23|44.53|0.27|13.93|13.35|

Select512 MB: both2160p FPS/p95/p99 ranges separate from256 MB, with mean+0.82 FPS and−1.46ms p95.
Late-frame share overlaps and the second pair is worse on that metric; do not claim a consistent late-frame reduction.
At1800p the mean+0.13 FPS is small, late frames overlap and no meaningful regression is demonstrated.
GPU time is unchanged at2160p and translation CPU rises0.17ms; reduced waiting, not reduced CPU work, is the supported explanation.
In the first2160p pair, cumulative staging-wait deltas over the measurement window fall from19.57/s to9.05/s.
The extra staging allocation costs256 MB. Neither resolution meets stable60 or <1% late frames.

Option: `env=NXRT_VK_STAGING_MB=512`. Undo: set256 or remove this override.
Evidence: `/tmp/claude-1000/bench/<run>/{frames.csv,Orbitals.log}` and `/tmp/claude-1000/orb/<run>_s.png`.
Recompute: `tools/dev4k/bench_stats.py --from 120 --to 180 <run-directory>`.

The final measured combination was installed at11:48, before the functional gameplay check:
DEFER1, SPLIT_MS2, WB_GPU1, WB_BASE_DEVICE1, FLIGHT0, STAGING_MB512 and the copied sync1 overlay.
The overlay is under the kit's `patches/night-sync1`; the original game pak is untouched.
The previous INI is backed up at desktop `orbitals-night-backup-20261126/measured-config-114822/Orbitals.ini`.
Restore that INI with the game closed to undo this performance configuration. Runtime code remains reversible
through the initial nightly executable/source backup; the selected device-memory option is off by default in code.
At close, input automation is disabled by removing `script_file` from the installed baseINI and neutralizing its input file.
The owner's original profile/settings and1620p choice are preserved, with gyroOff; all four owner save files match the initial
backup bySHA256. Restoring the initial root `Orbitals.ini` in the nightly backup restores the original configuration without
the benchmark input key. Test profiles and evidence remain separate for reproducibility.
