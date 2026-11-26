# test_profile.ps1 -On [-Tv "2160p (4K)"] | -Off | -Remove : a separate launcher profile for scripted test runs.
#   -On: creates (once) the profile "Night test" with a COPY of the active player profile's saves and its settings, sets the TV
#        resolution given and makes it the active profile (bench_launch.py / PLAY use the active profile). Remembers the
#        player's profile id in night-test-owner.txt.
#   -Off: makes the player's profile active again. -Remove: -Off, then deletes the test profile and its folder.
param([switch]$On, [switch]$Off, [switch]$Remove, [string]$Tv = "2160p (4K)", [string]$Fps = "60 (experimental)")
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$L = "$env:LOCALAPPDATA\OrbitalsLauncher"; $json = "$L\launcher.json"; $ownerFile = "$L\night-test-owner.txt"
$testId = "0badc0de0badc0de0badc0de0badc0de"
$d = Get-Content $json -Raw | ConvertFrom-Json
if ($On) {
  $owner = if (Test-Path $ownerFile) { (Get-Content $ownerFile).Trim() } else { $d.active }
  if ($owner -eq $testId) { throw "the active profile is already the test profile and no owner id is stored" }
  Set-Content $ownerFile $owner
  $dir = "$L\profiles\$testId"
  if (-not (Test-Path "$dir\save")) {
    New-Item -ItemType Directory -Force "$dir\runs" | Out-Null
    Copy-Item -Recurse "$L\profiles\$owner\save" "$dir\save"
  }
  $settings = $d.profiles.$owner.settings | ConvertTo-Json -Depth 5 | ConvertFrom-Json
  $settings | Add-Member -Force NoteProperty tv_resolution $Tv
  $settings | Add-Member -Force NoteProperty fps $Fps
  $d.profiles | Add-Member -Force NoteProperty $testId ([pscustomobject]@{ name = "Night test"; settings = $settings })
  $d.active = $testId
  [IO.File]::WriteAllText($json, ($d | ConvertTo-Json -Depth 6))
  "test profile active ($Tv, $Fps), owner $owner"
}
if ($Off -or $Remove) {
  $owner = (Get-Content $ownerFile).Trim()
  $d.active = $owner
  if ($Remove) { $d.profiles.PSObject.Properties.Remove($testId) }
  [IO.File]::WriteAllText($json, ($d | ConvertTo-Json -Depth 6))
  if ($Remove) { Remove-Item -Recurse -Force "$L\profiles\$testId"; Remove-Item $ownerFile }
  "owner profile $owner active" + $(if ($Remove) { ", test profile removed" } else { "" })
}
