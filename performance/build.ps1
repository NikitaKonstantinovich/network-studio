param([string]$Build = 'build/performance', [switch]$Profile, [string[]]$Targets = @('network_studio_benchmark','network_studio','network_studio_cli'))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath 'VC/Auxiliary/Build/vcvars64.bat'
$lines = & cmd /c "`"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'MSVC initialization failed' }
foreach ($line in $lines) { if ($line -match '^([^=]+)=(.*)$' -and $matches[1] -ine 'Path') { [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process') } }
$compilerPath = $lines | Where-Object { $_ -match '^Path=' } | Select-Object -First 1
[Environment]::SetEnvironmentVariable('Path',$null,'Process')
[Environment]::SetEnvironmentVariable('PATH',($compilerPath -replace '^[^=]+=',''),'Process')
$ninja = Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
$buildPath = if ([IO.Path]::IsPathRooted($Build)) { $Build } else { Join-Path $root $Build }
$profiling = if ($Profile) { 'ON' } else { 'OFF' }
& cmake -S $root -B $buildPath -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release -DSTUDIO_BUILD_PERFORMANCE_TOOLS=ON "-DSTUDIO_PROFILE=$profiling" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$buildPath/Release"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& cmake --build $buildPath --config Release --parallel 4 --target $Targets
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
