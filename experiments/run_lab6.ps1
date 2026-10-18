param([string]$Output = '', [double]$StageDuration = 100, [double]$TestDuration = 200)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$cli = Join-Path $project 'build/Release/network_studio_cli.exe'
if (-not $Output) { $Output = Join-Path $project ('verification/lab6_' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
New-Item -ItemType Directory -Force -Path $Output | Out-Null
# These are documented local model units, not a conversion from NeuroNet radius.
$one = Get-Content -Raw -Encoding UTF8 (Join-Path $PSScriptRoot 'examples/lab6_1hz.json') | ConvertFrom-Json
$one.stages[0].duration = $StageDuration
$one.stages[1].duration = $StageDuration
$one.stages[2].duration = $TestDuration
$one.stages[3].duration = $StageDuration
$one.duration = $StageDuration
$one | ConvertTo-Json -Depth 24 | Set-Content -Encoding UTF8 (Join-Path $Output 'plan_1hz.json')
$ten = Get-Content -Raw -Encoding UTF8 (Join-Path $Output 'plan_1hz.json') | ConvertFrom-Json
foreach ($stage in $ten.stages) { if ($stage.stimulus_enabled) { $stage.frequency = 10 } }
$ten | ConvertTo-Json -Depth 24 | Set-Content -Encoding UTF8 (Join-Path $Output 'plan_10hz.json')
function Invoke-Experiment([string]$Name, [string[]]$Options) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $cli @Options --output (Join-Path $Output $Name) --checkpoint (Join-Path $Output ($Name + '.nscp')) *> (Join-Path $Output ($Name + '.log'))
    if ($LASTEXITCODE -ne 0) { throw "Experiment $Name failed; see its log" }
    $watch.Stop()
    "$Name : $($watch.Elapsed.TotalSeconds) wall seconds" | Add-Content (Join-Path $Output 'timing.txt')
}
Invoke-Experiment 'background' @('--config', (Join-Path $Output 'plan_1hz.json'), '--stage', '0')
$common = Join-Path $Output 'background.nscp'
Get-FileHash -Algorithm SHA256 -LiteralPath $common | Format-List | Out-File (Join-Path $Output 'common_checkpoint_sha256.txt')
foreach ($hz in @(1,10)) {
    $plan = Join-Path $Output "plan_${hz}hz.json"
    # Both branches start from this exact byte-identical state. Freeze/test and
    # retraining are explicit subsequent stages, with the same absolute clock.
    Invoke-Experiment "training_${hz}hz" @('--resume',$common,'--plan',$plan,'--stage','1')
    Invoke-Experiment "test_${hz}hz" @('--resume',(Join-Path $Output "training_${hz}hz.nscp"),'--stage','2')
    Invoke-Experiment "retraining_${hz}hz" @('--resume',(Join-Path $Output "test_${hz}hz.nscp"),'--stage','3')
}
Write-Output "Results: $Output"
