#!/usr/bin/env bash
# orb.sh launch | press "<p1 script>" ["<p2 script>"] | shot [name] | kill   (bench helpers for the gaming PC)
S=${ORB_OUT:-/tmp/claude-1000/orb}; mkdir -p "$S"
ps(){ enc=$(printf '%s' "$1" | iconv -t UTF-16LE | base64 -w0); ssh -o BatchMode=yes gamingpc "powershell -NoProfile -EncodedCommand $enc" 2>&1 | grep -v -e CLIXML -e '^<Objs'; }
case "$1" in
  launch) ps 'Start-ScheduledTask lz-orb-bench; Start-Sleep 3; Get-Process Orbitals -ErrorAction SilentlyContinue | % { "running $($_.Id)" }' ;;
  press) ps "\$t=\"step\`n$2\`n$3\"; [IO.File]::WriteAllText('C:\llm\tmp\orb\input.txt', \$t + \"\`n\")"; echo "sent p1='$2' p2='$3'" ;;
  shot) n=${2:-shot}; ps 'Start-ScheduledTask lz-shot-full; Start-Sleep 3' >/dev/null; scp -q gamingpc:C:/llm/tmp/shotfull.png "$S/$n.png" && python3 -c "
from PIL import Image; im=Image.open('$S/$n.png'); im.resize((1280,720)).save('$S/${n}_s.png')" && echo "$S/${n}_s.png" ;;
  kill) ps 'Get-Process Orbitals -ErrorAction SilentlyContinue | Stop-Process -Force; "killed"' ;;
esac
