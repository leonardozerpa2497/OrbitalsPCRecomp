#!/usr/bin/env bash
# log.sh <regex> [wait seconds]: matching lines of the newest session log on the gaming PC
cd "$(dirname "$0")"
body=$(printf '$env:PAT=%s; $env:WAIT=%s\n' "'${1//\'/\'\'}'" "${2:-0}"; cat lastlog.ps1)
enc=$(printf '%s' "$body" | iconv -t UTF-16LE | base64 -w0)
ssh -o BatchMode=yes gamingpc "powershell -NoProfile -EncodedCommand $enc" 2>&1 | grep -v -e CLIXML -e '^<Objs'
