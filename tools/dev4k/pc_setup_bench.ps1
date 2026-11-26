# one-time: the bench launch task (interactive, like lz-shot) and the live input file in the game's ini
$ProgressPreference='SilentlyContinue'
$kit = "$env:USERPROFILE\Desktop\OrbitalsPCRecomp-0.1.5"
New-Item -ItemType Directory -Force C:\llm\tmp\orb | Out-Null
Move-Item -Force "$env:TEMP\dev4k\tools__dev4k__bench_launch.py" C:\llm\tmp\orb\bench_launch.py
$pyw = "$env:LOCALAPPDATA\Programs\Python\Python313\pythonw.exe"
$x = Export-ScheduledTask -TaskName lz-shot
$x = $x -replace '<Command>[^<]*</Command>', "<Command>$pyw</Command>" -replace '<Arguments>[^<]*</Arguments>', "<Arguments>C:\llm\tmp\orb\bench_launch.py `"$kit`"</Arguments>"
Register-ScheduledTask -TaskName lz-orb-bench -Xml $x -Force | Out-Null
Set-Content C:\llm\tmp\orb\input.txt "idle`n`n"
$ini = "$kit\games\Orbitals\Orbitals.ini"
if (-not (Select-String -Path $ini -Pattern '^script_file=' -Quiet)) { Add-Content $ini "script_file=C:\llm\tmp\orb\input.txt" }
Get-Content $ini; (Get-ScheduledTask lz-orb-bench).Actions | Format-List Execute,Arguments | Out-String
