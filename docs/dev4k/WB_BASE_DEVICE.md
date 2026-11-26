# Dispatch write-back snapshots in device memory (`NXRT_VK_WB_BASE_DEVICE`)

Status: implemented, default off, **not measured on the PC yet**. Code: `runtime/nvn/vk/vk_wb_core.h` (ring, plans, post
room; no Vulkan, tested on the host by `tests/test_wb_core.c`), `runtime/nvn/vk/vk_mem.c` (`vkm_wb_prepare`, `wb_make`,
`wrd_init`, `gpu_wrote_base`, `wd_emit`), `runtime/nvn/vk/vk_submit.c` (`vks_post_reserve`),
`runtime/nvn/vk/vk_replay.c` (`dispatch`, `vkr_sub_begin`).

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
- One plan per dispatch range, made in `vkm_wb_prepare` before the shader is recorded (`wbp_prepare` / `wbp_plan` in
  `vk_wb_core.h`), in this order: device plan (snapshot in `g_wrd`, GPU comparison), host GPU plan (snapshot in the
  host ring, GPU comparison), host CPU plan (snapshot and the range after the shader in the host ring, CPU comparison),
  none (no snapshot: the whole range goes back, as before when the ring was full). A plan takes every piece of ring
  room it needs (snapshot, records, the wb_gpu 2 copy or the CPU plan's "after"), its landing record (`wb_t`, with the
  wb_gpu 2 check's block masks) and one completion post (`vks_post_reserve`), or it keeps none of them: a device plan
  that cannot be completed (forced by `NXRT_VK_WB_BASE_DEVICE_FAIL` before any room is taken, ring full, no memory for
  its record) is rewound before the host plan is tried, so it never costs the host plan its room. After the shader
  each range takes exactly the path planned for it; nothing there allocates, and nothing degrades to the whole range.
- Ring reclamation: a landing releases the ring up to the end it was posted with, and landings run in post order. A
  range's room is taken before the shader but its landing is posted after it, after the landings of earlier ranges
  and copies, so a plan is *held* from when its room is taken until its landing is posted (or the plan is given up:
  a range planned again by a later dispatch, `vkm_wb_drop` at a submission's start). A landing's end stops at the
  oldest held plan (`wring_end`), so the first landing of a dispatch can no longer release the room of the second
  range's records before that range lands (review finding: two 256 B ranges, B's records at 800, both landings ending
  at 1088). The tail never moves back (`wring_released`); `wr_land` publishes it after its last read.
- The ring must really be device memory: `vk_alloc_mem` may hand out system memory over the budget. `wrd_init` refuses
  anything with `sys` set or without a device-local type and keeps host snapshots (log: `[RED] wb_base_device: ...`).
- Write after read: the snapshot copies read what the shader writes. The dispatch waits for them (COPY -> COMPUTE
  execution dependency after the snapshot batch), in both modes (separate commit).
- wb_gpu 2 stays an independent oracle: with a device plan the host ring also gets a snapshot, and the CPU compares the
  GPU's records (made from the device snapshot) against its own word-by-word comparison of host copies.

## Switches and log
| env | meaning |
|---|---|
| `NXRT_VK_WB_BASE_DEVICE=1` | device snapshots (default 0) |
| `NXRT_VK_WB_BASE_DEVICE_FAIL=<n>` | test: every n-th range that could take a device snapshot gets the host one (1 = all) |

Log lines (every 60 frames when wb_gpu or the switch is on):
`wb_base per frame: snapshots X MB in host memory, Y MB in device memory, Z ranges given the host snapshot instead of the device one; so far F such (K forced), D device-snapshot write-backs dropped`.
At first use: `wb_base_device: dispatch snapshots in device memory (256 MB ring, wb_gpu N)`. The first 5 fallbacks
log their reason (`a range gets the host plan (...)`). D counts planned write-backs whose landing could not be posted;
with the post room reserved before the shader it must stay 0 (it can only move for a plan left over from another
submission, which `vkm_wb_drop` at a submission's start rules out). With wb_gpu 2: `wb_gpu per frame: ... checked N ranges so far, M blocks differing` (M must stay 0).

## PC test plan (parent runs it; nothing here has run on the PC)
1. `WB_GPU=2, WB_BASE_DEVICE=1`: `checked` grows, differing blocks 0, device MB > 0, dropped 0, picture normal.
2. Same plus `WB_BASE_DEVICE_FAIL=3`: host and device MB both > 0, forced count grows, differing 0, dropped 0.
3. `WB_GPU=1` A/B/A/B with and without `WB_BASE_DEVICE=1` (SPLIT_MS=2, DEFER=1, STAGING 256) at 1800p and 2160p;
   expect host snapshot MB/frame ~0 with the switch on. Performance claims only from those runs.

## Host tests
`cc -O1 -g -fsanitize=address,undefined -Iruntime/nvn/vk tests/test_wb_core.c -o /tmp/wbcore && /tmp/wbcore` (ends
`OK`). It runs the real `vk_wb_core.h` helpers: the review's two-range case (the pre-fix end rule overlaps, held
plans do not), 200 000 random dispatches mixing device, host GPU, host CPU and whole-range write-backs with the
completion thread landing at random points (no piece handed out over room a record still owes), transactional plans
(32 MiB range: forced or failed device plans leave the host plan the very same room; a random property check that a
device plan is never worse than the host plan alone), no-memory fallbacks, and the post room (reserved posts succeed
when realloc fails). Not covered: the Vulkan recording itself (barriers, descriptors), which only the PC run checks.

## Costs and caveats
- 256 MiB more device memory (counted in the device budget, so textures may be evicted sooner on small GPUs).
- Host ring room per range is unchanged (taken but unused in device mode), so ring pressure is the same as before.
- Separate, not done: landed pages are marked rewritten (`dm_guest_write_prepare`) and may be re-uploaded whole; see
  the report of this change before touching dirty tracking (per-word CPU writes must be kept).
