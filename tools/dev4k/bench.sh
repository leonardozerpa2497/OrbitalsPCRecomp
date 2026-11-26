#!/usr/bin/env bash
# bench.sh <tag> <seconds> <scene file> [ini line ...]: one bench run on the gaming PC.
#   Writes the scene's input script (tools/dev4k/scenes/*.txt, the script_file format: step name, P1 script, P2 script) to
#   C:\llm\tmp\orb\input.txt, puts the extra ini lines (e.g. env=NXRT_VK_DEFER=0) into the kit's Orbitals.ini between
#   "; bench begin" / "; bench end" markers (the launcher copies that ini into each session), starts the game like PLAY
#   (task lz-orb-bench, the launcher's active profile), waits <seconds>, closes it and copies the session's frames.csv and
#   Orbitals.log to $BENCH_OUT/<tag> (default /tmp/claude-1000/bench/<tag>). The markers are removed again at the end.
set -euo pipefail
tag=$1; secs=$2; scene=$3; shift 3
out=${BENCH_OUT:-/tmp/claude-1000/bench}/$tag; mkdir -p "$out"
ps(){ local enc; enc=$(printf '%s' "\$ProgressPreference='SilentlyContinue'; $1" | iconv -t UTF-16LE | base64 -w0)
      ssh -o BatchMode=yes gamingpc "powershell -NoProfile -EncodedCommand $enc" 2>&1 | grep -v -e CLIXML -e '^<Objs' || true; }
extra=""; for l in "$@"; do extra+="$l"$'\n'; done
scp -q "$scene" gamingpc:C:/llm/tmp/orb/input.txt
printf '%s' "$extra" > "$out/ini-extra.txt"; scp -q "$out/ini-extra.txt" gamingpc:C:/llm/tmp/orb/ini-extra.txt
ps '$ini="$env:USERPROFILE\Desktop\OrbitalsPCRecomp-0.1.5\games\Orbitals\Orbitals.ini"
Get-Process Orbitals -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1
$t=[IO.File]::ReadAllText($ini) -replace "(?s)\r?\n; bench begin.*?; bench end", ""
$x=[IO.File]::ReadAllText("C:\llm\tmp\orb\ini-extra.txt")
[IO.File]::WriteAllText($ini, $t.TrimEnd() + "`r`n; bench begin`r`n" + $x + "; bench end`r`n")
(Get-Item "C:\llm\tmp\orb\input.txt").LastWriteTime=Get-Date
Start-ScheduledTask lz-orb-bench' >/dev/null
echo "bench $tag: started $(date +%H:%M:%S), ${secs}s"
sleep "$secs"
ps '$ini="$env:USERPROFILE\Desktop\OrbitalsPCRecomp-0.1.5\games\Orbitals\Orbitals.ini"
Get-Process Orbitals -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
$t=[IO.File]::ReadAllText($ini) -replace "(?s)\r?\n; bench begin.*?; bench end\r?\n?", "`r`n"
[IO.File]::WriteAllText($ini, $t.TrimEnd() + "`r`n")
$r=Get-ChildItem "$env:LOCALAPPDATA\OrbitalsLauncher\profiles\*\runs\*" -Directory | Sort-Object LastWriteTime | Select-Object -Last 1
Set-Content C:\llm\tmp\orb\lastrun.txt $r.FullName; $r.FullName' | tail -1 > "$out/run-path.txt"
run=$(tr -d '\r' < "$out/run-path.txt"); rp=${run//\\//}; rp=${rp#C:/Users/Leonardo/}
for f in frames.csv Orbitals.log Orbitals.ini; do scp -q "gamingpc:$rp/$f" "$out/" 2>/dev/null || echo "missing $f"; done
echo "bench $tag: $run -> $out"
python3 "$(dirname "$0")/bench_stats.py" --from "${BENCH_FROM:-0}" --to "${BENCH_TO:-100000}" "$out:$tag"
