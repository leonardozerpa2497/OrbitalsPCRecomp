# Dispatch write-back snapshots in device memory (`NXRT_VK_WB_BASE_DEVICE`)

Status: implemented, default off, **not measured on the PC yet**. Code: `runtime/nvn/vk/vk_mem.c` (`vkm_wb_prepare`,
`wbd_plan`, `wrd_init`, `gpu_wrote_base`), `runtime/nvn/vk/vk_replay.c` (`dispatch`).

## What it changes
A compute dispatch that writes a pool range gets a snapshot of the range before the shader; after the shader only the
words that differ from the snapshot go back to guest memory (CPU words written meanwhile stay). With
`NXRT_VK_WB_GPU=1` the comparison runs on the GPU (`wb_diff.comp`), but the snapshot still lived in the host ring
(`g_wr`, HOST_VISIBLE|COHERENT|CACHED): in 2160p co-op ~500 ranges, ~150 MiB a frame were written to host memory over
PCIe and read back by the GPU, for ~20 K changed 64-word blocks (~5 MB of records) coming back.

`NXRT_VK_WB_BASE_DEVICE=1` (needs `NXRT_VK_WB_GPU>=1`) puts the snapshot in a 256 MiB DEVICE_LOCAL ring (`g_wrd`) at
the same offset as its room in the host ring (the host room is still taken, unused, so lifetimes are the host ring's).
The compact records stay in the host ring. `NXRT_VK_WB_GPU=2` keeps a host snapshot beside the device one, so the CPU
check compares the GPU's records (made from the device snapshot) against its own word-by-word comparison.

## Safety rules (why the code is shaped this way)
- Every resource of the GPU comparison (device ring, pipeline, record room, wb_gpu 2 copy room, alignment and range
  limits) is checked and reserved in `vkm_wb_prepare`, before the shader is recorded. A range that cannot take it gets
  the normal host snapshot there. After the shader a device-snapshot range has exactly one path (`wd_emit`); it never
  falls back to the CPU comparison (no host bytes) or to the whole range (would put back CPU words the shader left).
  If that path still fails (allocation of the landing record), the write-back is dropped and counted (`dropped`).
- The ring must really be device memory: `vk_alloc_mem` may hand out system memory over the budget. `wrd_init` refuses
  anything with `sys` set or without a device-local type and keeps host snapshots (log: `[RED] wb_base_device: ...`).
- Write after read: the snapshot copies read what the shader writes. The dispatch now waits for them (COPY ->
  COMPUTE execution dependency after the snapshot batch), in both modes; this was missing before (separate commit).

## Switches and log
| env | meaning |
|---|---|
| `NXRT_VK_WB_BASE_DEVICE=1` | device snapshots (default 0) |
| `NXRT_VK_WB_BASE_DEVICE_FAIL=<n>` | test: every n-th range that could take a device snapshot gets the host one (1 = all) |

Log lines (every 60 frames when wb_gpu or the switch is on):
`wb_base per frame: snapshots X MB in host memory, Y MB in device memory, Z ranges given the host snapshot instead of the device one; so far F such (K forced), D device-snapshot write-backs dropped`.
At first use: `wb_base_device: dispatch snapshots in device memory (256 MB ring, wb_gpu N)`. The first 5 fallbacks
log their reason. With wb_gpu 2: `wb_gpu per frame: ... checked N ranges so far, M blocks differing` (M must stay 0).

## PC test plan (parent runs it; nothing here has run on the PC)
1. `WB_GPU=2, WB_BASE_DEVICE=1`: `checked` grows, differing blocks 0, device MB > 0, dropped 0, picture normal.
2. Same plus `WB_BASE_DEVICE_FAIL=3`: host and device MB both > 0, forced count grows, differing 0, dropped 0.
3. `WB_GPU=1` A/B/A/B with and without `WB_BASE_DEVICE=1` (SPLIT_MS=2, DEFER=1, STAGING 256) at 1800p and 2160p;
   expect host snapshot MB/frame ~0 with the switch on. Performance claims only from those runs.

## Costs and caveats
- 256 MiB more device memory (counted in the device budget, so textures may be evicted sooner on small GPUs).
- Host ring room per range is unchanged (taken but unused in device mode), so ring pressure is the same as before.
- Separate, not done: landed pages are marked rewritten (`dm_guest_write_prepare`) and may be re-uploaded whole; see
  the report of this change before touching dirty tracking (per-word CPU writes must be kept).
