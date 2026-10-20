param(
    [ValidateSet('Short','Laboratory')][string]$Scenario = 'Short',
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$Python = 'python',
    [string]$Seeds = '42',
    [switch]$Run
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$cli = Join-Path $project 'build/Release/network_studio_cli.exe'
$destinationPath = [IO.Path]::GetFullPath($Destination)
$labsPath = [IO.Path]::GetFullPath((Join-Path $project '../labs')).TrimEnd('\')
if ($destinationPath -eq $labsPath -or $destinationPath.StartsWith($labsPath + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'labs is read-only' }
if (Test-Path -LiteralPath $destinationPath) { throw 'Choose a new destination; existing results are preserved' }
New-Item -ItemType Directory -Path $destinationPath | Out-Null
$scenarioPath = Join-Path $destinationPath 'scenario'
$packagePath = Join-Path $destinationPath 'package'
$resultsPath = Join-Path $destinationPath 'results'
$arguments = @('-X','utf8',(Join-Path $PSScriptRoot 'make_scenario.py'),'--cli',$cli,'--destination',$scenarioPath,'--seeds',$Seeds)
if ($Scenario -eq 'Laboratory') { $arguments += '--laboratory' }
& $Python @arguments
if ($LASTEXITCODE -ne 0) { throw 'Scenario preparation failed' }
& $Python -X utf8 (Join-Path $PSScriptRoot 'package.py') create --spec (Join-Path $scenarioPath 'spec.json') --destination $packagePath --cli $cli
if ($LASTEXITCODE -ne 0) { throw 'Package validation failed' }
$runner = Join-Path $packagePath 'runtime/package.py'
if ($Run) {
    & $Python -X utf8 $runner run --package $packagePath --output $resultsPath
    if ($LASTEXITCODE -ne 0) { throw 'Experiment failed; inspect report.json' }
} else {
    Write-Output 'Prepared and validated; no simulation performed.'
    Write-Output ('Run explicitly: & "{0}" -X utf8 "{1}" run --package "{2}" --output "{3}"' -f $Python,$runner,$packagePath,$resultsPath)
}
