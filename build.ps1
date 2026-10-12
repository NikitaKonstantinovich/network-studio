param([switch]$Test, [switch]$Smoke, [string]$SmokeOutput = '')
$ErrorActionPreference = 'Stop'
$buildDir = Join-Path $PSScriptRoot 'build'
$cmakeBuild = Join-Path $buildDir 'native'
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
$sdk = Get-ChildItem -LiteralPath $sdkRoot -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um\Windows.h') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'Windows 10/11 SDK was not found' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'MSVC C++ Build Tools were not found' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$environmentLines = & cmd /c "`"$vcvars`" $($sdk.Name) >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'MSVC environment initialization failed' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$' -and $matches[1] -ine 'Path') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$compilerPathLine = $environmentLines | Where-Object { $_ -match '^Path=' } | Select-Object -First 1
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('PATH', ($compilerPathLine -replace '^[^=]+=', ''), 'Process')
$ninja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (Test-Path -LiteralPath $ninja) {
    & cmake -S $PSScriptRoot -B $cmakeBuild -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$buildDir/Release"
} else {
    & cmake -S $PSScriptRoot -B $cmakeBuild -G 'NMake Makefiles' -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$buildDir/Release"
}
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& cmake --build $cmakeBuild --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
if ($Test) {
    & ctest --test-dir $cmakeBuild -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed' }
}
if ($Smoke) {
    $exe = Join-Path $buildDir 'Release\network_studio.exe'
    $report = if (-not $SmokeOutput) { Join-Path $PSScriptRoot 'verification\gui' }
              elseif ([IO.Path]::IsPathRooted($SmokeOutput)) { $SmokeOutput }
              else { Join-Path $PSScriptRoot $SmokeOutput }
    $process = Start-Process -FilePath $exe -ArgumentList @('--smoke', ('"' + $report + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "GUI smoke test failed; see $report\error.txt" }
}
Write-Output "Application: $buildDir\Release\network_studio.exe"
