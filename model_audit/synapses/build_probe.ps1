$ErrorActionPreference = 'Stop'
$auditRoot = $PSScriptRoot
$studioRoot = (Resolve-Path (Join-Path $auditRoot '../..')).Path
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Include'
$sdk = Get-ChildItem -LiteralPath $sdkRoot -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'um/Windows.h') } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vsPath 'VC/Auxiliary/Build/vcvars64.bat'
$environmentLines = & cmd /c "`"$vcvars`" $($sdk.Name) >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'MSVC environment initialization failed' }
foreach ($line in $environmentLines) {
    if ($line -match '^([^=]+)=(.*)$' -and $matches[1] -ine 'Path') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$compilerPathLine = $environmentLines | Where-Object { $_ -match '^Path=' } | Select-Object -First 1
[Environment]::SetEnvironmentVariable('Path', $null, 'Process')
[Environment]::SetEnvironmentVariable('PATH', ($compilerPathLine -replace '^[^=]+=', ''), 'Process')
Push-Location $auditRoot
try {
    & cl /nologo /O2 /std:c++17 /EHsc /utf-8 /MT "/I$studioRoot" probe.cpp "$studioRoot/build/native/network_core.lib" /Fe:probe.exe /Fo:probe.obj
    if ($LASTEXITCODE -ne 0) { throw 'Harness compilation failed' }
    & ./probe.exe ./raw
    if ($LASTEXITCODE -ne 0) { throw 'Harness failed' }
} finally { Pop-Location }
