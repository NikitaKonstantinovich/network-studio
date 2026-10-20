param()
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location -LiteralPath $root
& powershell -ExecutionPolicy Bypass -File performance/build.ps1 -Build build/parallel-profile -Profile -Targets network_studio_lab_benchmark *> performance/20261020_lab6/build-profile-optimized.log
if ($LASTEXITCODE -ne 0) { throw 'Profiling build failed' }
$dest = 'performance/20261020_lab6/validation/profiles'
if (Test-Path -LiteralPath $dest) { throw 'Profile destination already exists' }
New-Item -ItemType Directory -Path $dest | Out-Null
$exe = 'build/parallel-profile/Release/network_studio_lab_benchmark.exe'
foreach ($n in @(1,8)) {
    & $exe ../labs/lab6_config_initial.json "$dest/profile$n-quiet" $n 0 0.05
    if ($LASTEXITCODE -ne 0) { throw 'Quiet profile failed' }
    & $exe "$dest/profile$n-quiet/final.nscp" "$dest/profile$n-burst" $n 0.05 0.1
    if ($LASTEXITCODE -ne 0) { throw 'Burst profile failed' }
}
& performance/20261020_lab6/baseline-release/network_studio_lab_profile.exe performance/20261020_lab6/pilot/profile8-burst/final.nscp performance/20261020_lab6/pilot/profile8-long 8 0.1 0.5
if ($LASTEXITCODE -ne 0) { throw 'Baseline long profile failed' }
& $exe "$dest/profile8-burst/final.nscp" "$dest/profile8-long" 8 0.1 0.5
if ($LASTEXITCODE -ne 0) { throw 'Optimized long profile failed' }
