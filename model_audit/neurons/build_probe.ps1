$ErrorActionPreference='Stop'
$studioDir=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsPath=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars=Join-Path $vsPath 'VC/Auxiliary/Build/vcvars64.bat'
$environmentLines=& cmd /c "`"$vcvars`" >nul && set"
if($LASTEXITCODE -ne 0){throw 'MSVC environment failed'}
foreach($line in $environmentLines){
    if($line -match '^([^=]+)=(.*)$' -and $matches[1] -ine 'Path'){
        [Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')
    }
}
$compilerPathLine=$environmentLines | Where-Object {$_ -match '^Path='} | Select-Object -First 1
[Environment]::SetEnvironmentVariable('Path',$null,'Process')
[Environment]::SetEnvironmentVariable('PATH',($compilerPathLine -replace '^[^=]+=',''),'Process')
$ninja=Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'
& cmake -S $PSScriptRoot -B (Join-Path $PSScriptRoot 'build') -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release '-DCMAKE_CXX_FLAGS_DEBUG=/Od'
if($LASTEXITCODE -ne 0){throw 'Diagnostic configuration failed'}
& cmake --build (Join-Path $PSScriptRoot 'build') --config Release
if($LASTEXITCODE -ne 0){throw 'Diagnostic build failed'}
& (Join-Path $PSScriptRoot 'build/neuron_probe.exe') (Join-Path $PSScriptRoot 'probe.csv') | Set-Content -LiteralPath (Join-Path $PSScriptRoot 'probe_bridge.json') -Encoding utf8
if($LASTEXITCODE -ne 0){throw 'Diagnostic probe failed'}
