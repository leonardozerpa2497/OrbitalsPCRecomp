# prints lines of the newest session's Orbitals.log matching $env:PAT (after waiting $env:WAIT seconds)
$ProgressPreference='SilentlyContinue'
if ($env:WAIT) { Start-Sleep ([int]$env:WAIT) }
$runs = Get-ChildItem "$env:LOCALAPPDATA\OrbitalsLauncher\profiles\*\runs\*" -Directory | Sort-Object LastWriteTime
$last = $runs | Select-Object -Last 1
"run $($last.Name)"; Get-Process Orbitals -ErrorAction SilentlyContinue | ForEach-Object { "running $($_.Id)" }
Select-String -Path "$($last.FullName)\Orbitals.log" -Pattern $env:PAT | Select-Object -First 60 | ForEach-Object { $_.Line.Substring(0,[Math]::Min(400,$_.Line.Length)) }
