param([string]$Tag = 'v2026.11.21', [string]$Output = '', [string]$ReleaseDirectory = '')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if ($Tag -notmatch '^v[0-9][0-9A-Za-z._-]*$') { throw 'Invalid release tag' }
$taskCommit = (& git -C $taskRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve source commit' }
$taskDirty = & git -C $taskRoot status --porcelain --untracked-files=no
if ($taskDirty) { throw 'Commit tracked source changes before packaging' }
$taskOut = if ($Output) { [IO.Path]::GetFullPath($Output) } else { Join-Path $taskRoot 'build\packages' }
$taskBinaries = if ($ReleaseDirectory) { [IO.Path]::GetFullPath($ReleaseDirectory) } else { Join-Path $taskRoot 'build\Release' }
$taskName = "network-studio-$Tag-windows-x64"
$taskStage = Join-Path $taskOut $taskName
$taskZip = Join-Path $taskOut ($taskName + '.zip')
if ((Test-Path -LiteralPath $taskStage) -or (Test-Path -LiteralPath $taskZip)) { throw 'Package output already exists; choose a fresh Output directory' }
foreach ($taskExe in @('network_studio.exe','network_studio_cli.exe','network_studio_population.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $taskBinaries $taskExe) -PathType Leaf)) { throw "Build Release first: $taskExe missing in $taskBinaries" }
}
$taskBuildJson = & (Join-Path $taskBinaries 'network_studio_cli.exe') --package-build-info
if ($LASTEXITCODE -ne 0) { throw 'CLI build metadata failed' }
$taskBuild = ($taskBuildJson -join "`n") | ConvertFrom-Json
New-Item -ItemType Directory -Path (Join-Path $taskStage 'build\Release') -Force | Out-Null
foreach ($taskExe in @('network_studio.exe','network_studio_cli.exe','network_studio_population.exe')) {
    Copy-Item -LiteralPath (Join-Path $taskBinaries $taskExe) -Destination (Join-Path $taskStage 'build\Release')
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'launch.cmd') -Destination $taskStage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.txt') -Destination $taskStage
foreach ($taskDirectory in @('examples','docs')) { Copy-Item -LiteralPath (Join-Path $taskRoot $taskDirectory) -Destination $taskStage -Recurse }
New-Item -ItemType Directory -Path (Join-Path $taskStage 'neuroanimat') | Out-Null
foreach ($taskItem in @('examples','images','README_RU.md','REPORT_RU.md','run_demo.py','verify_demo.py','migrate_activity_checkpoint.py')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot "neuroanimat\$taskItem") -Destination (Join-Path $taskStage 'neuroanimat') -Recurse
}
$taskMetadata = [ordered]@{
    release=$Tag; source_commit=$taskCommit; repository='https://github.com/NikitaKonstantinovich/network-studio'
    platform='Windows x64'; packaged_utc=(Get-Date).ToUniversalTime().ToString('o'); numerical_build=$taskBuild
    executables=@(Get-ChildItem -LiteralPath (Join-Path $taskStage 'build\Release') -File | ForEach-Object {
        [ordered]@{path=('build/Release/' + $_.Name); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
}
$taskMetadata | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $taskStage 'BUILD_INFO.json') -Encoding utf8
@("# Neuro / Astro - Network Studio $Tag",'', 'Windows x64. Extract the entire ZIP, then run launch.cmd.', '',
    '[Installation (Russian)](docs/INSTALL_RU.md) | [GUI/CLI guide (Russian)](docs/USER_GUIDE_RU.md)', '',
    "[Release source](https://github.com/NikitaKonstantinovich/network-studio/tree/$Tag)", '',
    'GUI/CLI: build/Release. Examples: examples and neuroanimat/examples.',
    'BUILD_INFO.json records the source commit, numerical build and executable SHA-256.',
    'CHECKSUMS.sha256 covers all package files except itself.',
    'Checkpoints require a compatible build. GUI/CLI require neither CUDA nor Python.') | Set-Content -LiteralPath (Join-Path $taskStage 'README.md') -Encoding utf8
$taskLines = Get-ChildItem -LiteralPath $taskStage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $taskRelative = $_.FullName.Substring($taskStage.Length+1).Replace('\','/')
    (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $taskRelative
}
$taskLines | Set-Content -LiteralPath (Join-Path $taskStage 'CHECKSUMS.sha256') -Encoding utf8
Compress-Archive -LiteralPath $taskStage -DestinationPath $taskZip -CompressionLevel Optimal
Copy-Item -LiteralPath (Join-Path $taskStage 'BUILD_INFO.json') -Destination (Join-Path $taskOut 'BUILD_INFO.json')
@($taskZip,(Join-Path $taskOut 'BUILD_INFO.json')) | ForEach-Object {
    (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($_)
} | Set-Content -LiteralPath (Join-Path $taskOut 'SHA256SUMS.txt') -Encoding ascii
Write-Output $taskZip
