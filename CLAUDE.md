# Orbitals PC port — local work (fork of Dimi-Sama/OrbitalsPCRecomp)

Reply to the owner in neutral Latin American Spanish; code, commits and docs in English. Global rules:
~/.claude-20x/CLAUDE.md. Read docs/dev4k/OVERNIGHT_PLAN.md before changing the runtime or the launcher. The game runs
on the gaming PC (`ssh gamingpc`); tools/dev4k/ builds, installs and drives it. Never commit game data, keys or saves.

## Overnight
```
gate:              x86_64-w64-mingw32-gcc -fsyntax-only on touched runtime .c files; python -m py_compile on touched .py;
                   then tools/dev4k/push.sh <files> builds with no errors and the game boots to the title (log + shot)
env:               none
preview:           screenshots via tools/dev4k/orb.sh shot (scheduled task lz-shot-full); no ports
shots:             3840 (full) and 1280 (scaled)
integration:       none: night branch only (no push)
test DB:           n/a
dev DB:            n/a
shared rows:       the owner's save: only a copy in a separate launcher test profile; his profile active at close
backlog:           docs/dev4k/OVERNIGHT_PLAN.md (goals A perf, B gyro, C button layout; all three required)
review:            1 reviewer of the other provider (Codex) per M/L item touching runtime/ or runtime/nvn/
metrics:           co-op bench: % frames > 17.7 ms, p50/p95, FPS, translation ms/frame, GPU ms/frame, >= 2 alternated runs
deploy notes:      what is installed on the PC, which ini/launcher options, how to restore from the night's backup folder
paid APIs:         none
```
