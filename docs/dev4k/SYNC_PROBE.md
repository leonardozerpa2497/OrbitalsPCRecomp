# Frame synchronization experiment

Keep the game's default `r.GTSyncType=2` until an alternative passes the alternating benchmark and stability checks. Setting it to zero produced a confirmed stopped frame counter in the observed run, not a usable performance result.

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

The game's embedded help distinguishes mode 0 (render N−1, RHI N−2), mode 1 (RHI N−1), and mode 2 (swapchain flip).
Mode1 probe `device-syncpak1-2160-probe` completed: effective CVar1/1 confirmed by `/tmp/orbitals-syncpak1-live.txt`,
same co-op scene visually confirmed at125s, and3269 frames in the120–180s measurement window. It gave54.51 FPS,
p5017.30 ms, p9524.73 ms, p9929.16 ms,44.40% late,0.70% above30 ms, translation CPU13.75 ms and GPU13.33 ms.
This is a promising single run, not an accepted optimization or proof of long-session stability. The alternating
mode2/1 comparison at1800p and2160p is next; both variants use the same PlayStation layout and FLIGHT0 configuration.

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
