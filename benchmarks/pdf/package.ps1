param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$MsysRoot
)
$ErrorActionPreference = 'Stop'
$benchmarkBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $benchmarkBuild 'speedynote.exe') -PathType Leaf)) {
    throw 'Missing speedynote.exe; build and deploy the Release executable first.'
}
$cache = Join-Path $benchmarkBuild 'CMakeCache.txt'
if (-not (Select-String -LiteralPath $cache -Pattern '^SPEEDYNOTE_ENABLE_PDF_BENCHMARK:BOOL=ON$' -Quiet)) {
    throw 'The build does not enable SPEEDYNOTE_ENABLE_PDF_BENCHMARK.'
}
Copy-Item -LiteralPath $cache -Destination (Join-Path $benchmarkBuild 'CMakeCache.benchmark.txt') -Force
$pacman = Join-Path $MsysRoot 'usr/bin/pacman.exe'
$packages = & $pacman -Q
if ($LASTEXITCODE -ne 0) { throw 'Cannot record MSYS2 package versions.' }
$packages | Set-Content -LiteralPath (Join-Path $benchmarkBuild 'msys2-packages.txt') -Encoding utf8
$revision = & git -C $PSScriptRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source revision.' }
$changes = & git -C $PSScriptRoot status --porcelain --untracked-files=normal
if ($LASTEXITCODE -ne 0) { throw 'Cannot record source state.' }
[ordered]@{
    source_revision = $revision.Trim()
    source_status = @($changes)
    executable_sha256 = (Get-FileHash -LiteralPath (Join-Path $benchmarkBuild 'speedynote.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    purpose = 'Compile artifact only; no desktop performance baseline has been measured.'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $benchmarkBuild 'benchmark-build.json') -Encoding utf8
$destination = Join-Path $benchmarkBuild 'benchmarks/pdf'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($name in @('bench.mjs', 'bench.test.mjs', 'pan-suite.mjs', 'README.md', 'check-render-lifecycle.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $destination -Force
}
