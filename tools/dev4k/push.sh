#!/usr/bin/env bash
# push.sh <repo-relative files...>: stage them on the gaming PC and rebuild the runtime (pc_build.ps1)
set -euo pipefail
cd "$(dirname "$0")/../.."
ssh -o BatchMode=yes gamingpc 'New-Item -ItemType Directory -Force $env:TEMP\dev4k | Out-Null' >/dev/null
for f in "$@"; do scp -q "$f" "gamingpc:AppData/Local/Temp/dev4k/${f//\//__}"; done
enc=$(iconv -t UTF-16LE tools/dev4k/pc_build.ps1 | base64 -w0)
ssh -o BatchMode=yes gamingpc "powershell -NoProfile -EncodedCommand $enc" 2>&1 | grep -v -e CLIXML -e '^<Objs'
