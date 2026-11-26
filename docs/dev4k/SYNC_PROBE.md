# Frame synchronization experiment

Keep the game's default `r.GTSyncType=2` until an alternative passes a real gameplay run. Setting it to zero produced a reproducible stopped frame counter in the observed run, not a usable performance result.

## What was measured

The initial Saved/Config Engine.ini overlay did not change the effective value: live reads still returned 2 for both game and render threads. Its unchanged FPS therefore did not test the hypothesis.

The subsequent `device-syncpak0-2160-probe` used a copied pak overlay changing only the device profile's `r.GTSyncType=2` to `0`. Live reads confirmed 0/0; Kepler FrameSyncType remained -1/-1 and OneFrameThreadLag remained 1/1. The present CSV stops at frame 4966, 87.085 seconds. Watchdog dumps at 94.55, 110.01, 125.82 and 141.45 seconds retain that counter. Screens at approximately 125 seconds and later show the same frozen cinematic. There are no frames in the intended 120–180 second measurement window: no FPS comparison is valid.

Evidence: `/tmp/claude-1000/bench/device-syncpak0-2160-probe/`, `/tmp/orbitals-syncpak0-live.txt`, and `/tmp/claude-1000/orb/device-syncpak0-{2160-probe,late}_s.png`. The temporary overlay was removed after the probe. The original pak was not overwritten.

## What the saved stacks establish

Offline binary analysis identifies two persistent waits:

- Guest thread 111 waits for a task used by scene visibility. Its caller loads the task from `x23+0x20d0`; the wait checks the completion bit at task+0xa0.
- Guest thread 138 requests a blocking occlusion-query result. With no result and no completion task on the query, the NVN2 implementation forces an RHI flush and waits for a TaskGraph dispatch task at commandList+0x2c0. This wait occurs before reading the GPU result.

This suggests a visibility/RHI task dependency exposed by extra overlap, but does **not** prove a dependency cycle or locate the runtime fault. Task identities, prerequisites and host translation/completion/present stacks are missing. The observed command-list global is 0x11b56a60; validate its identity before interpreting pending-task field 0x11b56d20.

## Bounded next experiment

The game's embedded help distinguishes mode 0 (render N−1, RHI N−2), mode 1 (RHI N−1), and mode 2 (swapchain flip). Mode 1 is a candidate, not an accepted fix. A local mode-1 overlay has been prepared but has not run. Before interpreting its FPS, confirm effective CVar 1/1, successful arrival in the same co-op scene, continued frames throughout the measurement window and unchanged images. Any retained change still requires two alternating runs per target resolution.

Undo any sync experiment by removing `env=SWR_ROMFS_EXTRA_OVERLAY=...` from the test INI. Do not change the owner profile or replace the original game pak.
