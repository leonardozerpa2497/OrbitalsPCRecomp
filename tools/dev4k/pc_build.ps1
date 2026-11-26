# dev loop for the display-4k experiments: files staged in %TEMP%\dev4k\<path with __ for \> replace the kit's,
# then the NVN layer and the runtime are rebuilt and installed as games\Orbitals\Orbitals.exe (the game must be closed).
$ProgressPreference='SilentlyContinue'; $ErrorActionPreference='Stop'
$repo = "$env:USERPROFILE\Desktop\OrbitalsPCRecomp-0.1.5"; Set-Location $repo
$b = "$repo\backup-runtime-0.1.5"
Get-Process Orbitals -ErrorAction SilentlyContinue | Stop-Process -Force
Get-ChildItem "$env:TEMP\dev4k" -File | ForEach-Object {
  $rel = $_.Name -replace '__','\'; $dst = Join-Path $repo $rel
  $keep = Join-Path $b ($rel -replace '\\','__')
  if ((Test-Path $dst) -and -not (Test-Path $keep)) { Copy-Item $dst $keep }
  Move-Item -Force $_.FullName $dst; "staged $rel" }
$gbin = "$repo\gcc\Library\bin"; $tc = "$repo\games\toolchain"
$env:PATH = "$gbin;$tc\vulkan-bin;$env:PATH"; $env:VULKAN_SDK = "$repo\runtime\edenvk\deps\vulkan-headers"
$ErrorActionPreference='Continue'
& python\python.exe -B tools\build_nvn.py --jobs 16 *> "$env:TEMP\dev4k_nvn.log"; "build_nvn exit $LASTEXITCODE"
if ($LASTEXITCODE) { Get-Content "$env:TEMP\dev4k_nvn.log" | Select-String -Pattern 'error' | Select-Object -First 15 | ForEach-Object { $_.Line }; exit 1 }
& python\python.exe -B switchrecomp.py --build-runtime --cc "$gbin\x86_64-w64-mingw32-gcc.exe" --target win64 --backend win32 *> "$env:TEMP\dev4k_rt.log"; "runtime exit $LASTEXITCODE"
if ($LASTEXITCODE) { Get-Content "$env:TEMP\dev4k_rt.log" | Select-String -Pattern 'error' | Select-Object -First 15 | ForEach-Object { $_.Line }; exit 1 }
Copy-Item -Force bin\runtime_win64_win32.exe games\Orbitals\Orbitals.exe; "installed"
