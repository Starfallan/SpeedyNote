param([Parameter(Mandatory)][string]$BuildDirectory)
# Optional benchmark-only Step3 executable with the current measurement probe.
# Run after the candidate is built/deployed and before deleting build files.
$ErrorActionPreference = 'Stop'
$referenceRevision = '37050039666e084db158c0393964e23e081df869'
$referenceRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$referenceBuild = (Resolve-Path -LiteralPath $BuildDirectory).Path
$candidatePath = Join-Path $referenceBuild 'speedynote.exe'
$savedCandidatePath = Join-Path $referenceBuild 'speedynote-candidate.saved.exe'
$referencePath = Join-Path $referenceBuild 'speedynote-step3.exe'
if ((Test-Path -LiteralPath $savedCandidatePath) -or (Test-Path -LiteralPath $referencePath)) {
    throw 'Reference output already exists.'
}
$referenceFiles = @(
    'source/core/DocumentViewport.cpp', 'source/core/DocumentViewport.h',
    'source/core/PdfRenderLifecycleTests.h', 'source/objects/TextBoxObject.cpp'
)
$referenceBackups = @{}
$referenceSources = @{}
$referenceCandidateRevision = & git -C $referenceRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot read candidate revision.' }
# A shallow Actions checkout may not contain Step3. Fetch only that commit.
& git -C $referenceRoot fetch --no-tags --depth=1 origin $referenceRevision
if ($LASTEXITCODE -ne 0) { throw 'Cannot fetch the Step3 reference.' }
$allowedFiles = $referenceFiles + @('source/benchmarks/PdfBenchmark.cpp', 'source/benchmarks/PdfBenchmarkSupport.h')
$changedFiles = & git -C $referenceRoot diff --name-only $referenceRevision HEAD -- source CMakeLists.txt
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect reference scope.' }
if (@($changedFiles | Where-Object { $_ -notin $allowedFiles }).Count -ne 0) {
    throw 'Application changes exceed this reference recipe; update it before building a new reference.'
}
foreach ($relative in $referenceFiles) {
    $referenceFile = Join-Path $referenceRoot $relative
    $referenceBackups[$relative] = [IO.File]::ReadAllBytes($referenceFile)
    $referenceText = & git -C $referenceRoot show "${referenceRevision}:$relative"
    if ($LASTEXITCODE -ne 0) { throw "Cannot read reference file: $relative" }
    $referenceSources[$relative] = ($referenceText -join "`n") + "`n"
}
Copy-Item -LiteralPath $candidatePath -Destination $savedCandidatePath
try {
    foreach ($relative in $referenceFiles) {
        [IO.File]::WriteAllText((Join-Path $referenceRoot $relative), $referenceSources[$relative],
            [Text.UTF8Encoding]::new($false))
    }
    & cmake --build $referenceBuild --config Release -- -j4
    if ($LASTEXITCODE -ne 0) { throw 'Step3 reference compilation failed.' }
    Copy-Item -LiteralPath $candidatePath -Destination $referencePath
    [ordered]@{
        base_revision = $referenceRevision
        probe_revision = $referenceCandidateRevision.Trim()
        restored_source_files = $referenceFiles
        executable_sha256 = (Get-FileHash -LiteralPath $referencePath -Algorithm SHA256).Hash.ToLowerInvariant()
        purpose = 'Step3 application with current PDF measurement probe; manual A/B only'
    } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $referenceBuild 'step3-reference-build.json') -Encoding utf8
} finally {
    foreach ($relative in $referenceFiles) {
        [IO.File]::WriteAllBytes((Join-Path $referenceRoot $relative), $referenceBackups[$relative])
    }
    Copy-Item -LiteralPath $savedCandidatePath -Destination $candidatePath -Force
    Remove-Item -LiteralPath $savedCandidatePath
}
