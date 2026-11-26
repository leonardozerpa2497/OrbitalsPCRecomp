# Translation-thread profile and rejected shortcuts

Evidence: `/tmp/claude-1000/bench/profile2160/`, including the captured executable, `profile-0180.txt.focus.txt`,
`symbolized.txt` and `Orbitals.log`. Reproduce symbolization with
`python3 tools/claude_laptop/prof_symbolize.py <run directory> profile-0180.txt`.
This diagnostic run enables PHASES/SETSTATS and is not a final-build FPS comparison.

The focus-thread sample contains2708 active samples and1645 waiting samples. Of active samples,30.24% are outside
the runtime with `guest_copy` as the identified runtime caller, plus0.52% inside `guest_copy` itself. This is the
memory-copy path, not an unexplained shader cost. Other self-time samples include `write_set_`7.68%, `vkm_use_`4.91%,
`write_set`3.95%, `draw`3.69%, `gpu_pending`2.84%, `vkm_use`2.58%, `pool_sync`2.33%, emulated TLS2.10% and index-buffer
binding1.62%. These samples locate work; they are not additive promises of recoverable frame time.

SETSTATS reports roughly6850 descriptor requests per frame,29.4% seen with exact inputs in this or the previous frame,
and about77.5% when UBO addresses are reduced to size/1-MB-region keys. Existing consecutive reuse already handles
about28.7%; an exact-input cache therefore has little measured headroom. Dynamic UBO offsets offer a larger hypothesis,
but require layout/pipeline changes and a cache bounded by real descriptor-pool/submission lifetimes. The77.5% number
also includes previous-frame matches; it is not an achievable hit rate proven for the current pool lifetime.

## Why writeback pages cannot simply be marked clean

The current shadow is not guaranteed to be a complete image of device memory. A concrete counterexample from code
analysis: upload `[0,0]`; the CPU then writes the first word, giving guest `[1,0]`; a later GPU-pending snapshot captures
that guest state. The GPU changes only the second word to7. Differential writeback leaves guest and shadow `[1,7]`,
while device memory remains `[0,7]`. Comparing guest against that shadow and suppressing the upload loses the CPU's1.
The existing whole-page upload correctly sends it. Clearing dirty bits on landing has the same missing-proof problem.

A safe optimization would need a baseline captured from exactly the bytes sent to the device, explicit page validity
and generation, invalidation for uncovered GPU writes/aliasing/pool recreation, and coordination with concurrent completion
callbacks. Keep the current full upload when that proof is absent. No such optimization was installed this run.

The accepted device-local snapshot change addresses a different cost: keeping the GPU comparison baseline in VRAM,
while preserving compact host output, shadows, masks, pending counts and completion ordering. Its measured benefit is
in [DEVICE_BASELINE_BENCH.md](DEVICE_BASELINE_BENCH.md); it does not eliminate the re-upload mechanism described here.
