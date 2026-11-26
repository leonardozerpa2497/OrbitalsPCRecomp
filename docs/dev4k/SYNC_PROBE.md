# Frame synchronization experiment

Select `r.GTSyncType=1` through the copied pak overlay for this PC: the completed alternating comparison below improves both resolutions outside the control spread. Setting it to zero produced a confirmed stopped frame counter and remains rejected. Mode1 has passed these bounded runs, not an all-level or long-session stability test.

## Completed alternating comparison

Eight 185-second runs, analysis120–180 seconds, same copied profile and `coop_ship_bottom.txt` scene.
PlayStation/bottom accept, gyroOff, DEFER1, SPLIT2, WB_GPU1, WB_BASE_DEVICE1, FLIGHT0, staging256 MB.
Only the sync overlay changes. Live reads at125s confirm the intended game/render CVar2/2 or1/1 in every run;
Kepler remains-1/-1 and OneFrameThreadLag1/1. All eight screenshots show the same scene without an obvious visual regression.

| Run | FPS | p50 ms | p95 ms | p99 ms | >17.7 ms % | >30 ms % | Translation CPU ms | GPU ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| sync-2-1800a |58.00|16.71|20.45|25.63|18.40|0.40|10.63|11.82|
| sync-1-1800a |58.61|16.67|19.52|22.47|14.51|0.00|10.42|11.83|
| sync-2-1800b |57.65|16.67|20.95|30.49|21.89|1.16|10.43|11.80|
| sync-1-1800b |58.51|16.67|19.63|22.29|14.59|0.09|10.45|11.71|
| sync-2-2160a |51.80|18.43|26.89|31.85|61.47|2.38|14.75|13.29|
| sync-1-2160a |54.75|17.23|24.25|28.70|43.48|0.67|13.68|13.32|
| sync-2-2160b |52.16|18.14|28.36|32.31|55.90|3.42|14.34|13.34|
| sync-1-2160b |54.93|17.14|24.42|28.99|41.37|0.55|13.71|13.29|

Mean gains:0.73 FPS at1800p and2.86 FPS at2160p. The p95 improves1.12/3.29 ms; late-frame share improves
5.60/16.26 percentage points. GPU duration is effectively unchanged; translation CPU improves about0.85 ms at2160p.
Both repetitions separate from their controls in FPS, p95 and late-frame share. Neither resolution meets stable60
or the requested <1% late frames. Input latency is NOT OBSERVED. Evidence is under
`/tmp/claude-1000/bench/<run>/`, `/tmp/<run>-live.txt`, and `/tmp/claude-1000/orb/<run>_s.png`.
Recompute with `tools/dev4k/bench_stats.py --from 120 --to 180 <run-directory>`.

## What was measured

The initial Saved/Config Engine.ini overlay did not change the effective value: live reads still returned 2 for both game and render threads. Its unchanged FPS therefore did not test the hypothesis.

The subsequent `device-syncpak0-2160-probe` used a copied pak overlay changing only the device profile's `r.GTSyncType=2` to `0`. Live reads confirmed 0/0; Kepler FrameSyncType remained -1/-1 and OneFrameThreadLag remained 1/1. The present CSV stops at frame 4966, 87.085 seconds. Watchdog dumps at 94.55, 110.01, 125.82 and 141.45 seconds retain that counter. Screens at approximately 125 seconds and later show the same frozen cinematic. There are no frames in the intended 120–180 second measurement window: no FPS comparison is valid.

Evidence: `/tmp/claude-1000/bench/device-syncpak0-2160-probe/`, `/tmp/orbitals-syncpak0-live.txt`, and `/tmp/claude-1000/orb/device-syncpak0-{2160-probe,late}_s.png`. The temporary overlay was removed after the probe. The original pak was not overwritten.

## What the saved stacks establish

Offline binary analysis identifies two persistent waits:

- Guest thread 111 waits for a task used by scene visibility. Its caller loads the task from `x23+0x20d0`; the wait checks the completion bit at task+0xa0.
- Guest thread 138 requests a blocking occlusion-query result. With no result and no completion task on the query, the NVN2 implementation forces an RHI flush and waits for a TaskGraph dispatch task at commandList+0x2c0. This wait occurs before reading the GPU result.

This suggests a visibility/RHI task dependency exposed by extra overlap, but does **not** prove a dependency cycle or locate the runtime fault. Task identities, prerequisites and host translation/completion/present stacks are missing. The observed command-list global is 0x11b56a60; validate its identity before interpreting pending-task field 0x11b56d20.

## Initial mode1 probe (superseded by the paired results above)

The game's embedded help distinguishes mode 0 (render N−1, RHI N−2), mode 1 (RHI N−1), and mode 2 (swapchain flip).
Mode1 probe `device-syncpak1-2160-probe` completed: effective CVar1/1 confirmed by `/tmp/orbitals-syncpak1-live.txt`,
same co-op scene visually confirmed at125s, and3269 frames in the120–180s measurement window. It gave54.51 FPS,
p5017.30 ms, p9524.73 ms, p9929.16 ms,44.40% late,0.70% above30 ms, translation CPU13.75 ms and GPU13.33 ms.
That single run was insufficient for acceptance; the subsequent alternating comparison above is the selection evidence.

Undo any sync experiment by removing `env=SWR_ROMFS_EXTRA_OVERLAY=...` from the test INI. Do not change the owner profile or replace the original game pak.

## Reproduce the overlay without overwriting game files

Use an extracted copy of the owned game's `Orbitals/Platforms/Switch2/Config/Switch2DeviceProfiles.ini`; change only
`r.GTSyncType=2` to `r.GTSyncType=1`, retaining encoding and line endings. The preserved helper takes a new output path:

```text
python3 tools/dev4k/pakpatch.py ORIGINAL.pak NEW.pak Orbitals/Platforms/Switch2/Config/Switch2DeviceProfiles.ini=EDITED_COPY.ini
```

It supports unencrypted, unsigned UE v11 paks and refuses an existing output. Place the resulting copy under the overlay's
`Orbitals/Content/Paks/Orbitals-Switch2.pak`, not over the original installation. No game data or keys are tracked in git.
The parent reproduced the test artifact from the original: SHA256
`ad895aa67baa3f693fbbe6912e924ff97b5e35807e2cd85ae4d5eec23958587f`; the edited INI differs by only that one value.
The helper's existing-destination rejection also preserved the prior output hash. This is artifact evidence, not a claim
that mode1 meets the performance or long-session-stability target. Extra overlap may change input latency; latency is not measured.
