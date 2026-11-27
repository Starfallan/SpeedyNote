param([Parameter(Mandatory)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
$lifecycleBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$lifecycleReport = Join-Path $lifecycleBuild 'pdf-render-lifecycle.json'
if (Test-Path -LiteralPath $lifecycleReport) {
    throw "Report already exists: $lifecycleReport"
}
# Use the Windows plugin already deployed with the portable package. The test
# never shows its viewports and does not produce performance measurements.
if (-not (Test-Path -LiteralPath (Join-Path $lifecycleBuild 'platforms/qwindows.dll') -PathType Leaf)) {
    throw 'Missing platforms/qwindows.dll; use the complete Windows portable package.'
}
$lifecycleProcess = Start-Process -FilePath (Join-Path $lifecycleBuild 'speedynote.exe') `
    -WorkingDirectory $lifecycleBuild `
    -ArgumentList @('--test-pdf-render-lifecycle', 'pdf-render-lifecycle.json', '-platform', 'windows') `
    -WindowStyle Hidden -PassThru
if (-not $lifecycleProcess.WaitForExit(60000)) {
    $lifecycleProcess.Kill()
    throw 'PDF lifecycle tests exceeded 60 seconds.'
}
$lifecycleProcess.Refresh()
if (-not (Test-Path -LiteralPath $lifecycleReport)) {
    throw "PDF lifecycle tests did not write a report (exit $($lifecycleProcess.ExitCode))."
}
Get-Content -LiteralPath $lifecycleReport
$lifecycleResult = Get-Content -LiteralPath $lifecycleReport -Raw | ConvertFrom-Json
if ($lifecycleProcess.ExitCode -ne 0 -or $lifecycleResult.passed -ne $true) {
    throw 'PDF lifecycle tests failed.'
}
