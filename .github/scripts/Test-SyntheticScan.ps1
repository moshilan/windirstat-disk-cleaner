# Read-only application smoke test. All input is newly generated under the runner temp root.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
if (-not $env:RUNNER_TEMP) { throw 'RUNNER_TEMP is required; do not scan local user directories.' }
$testRoot = Join-Path $env:RUNNER_TEMP ('disk-cleaner-smoke-' + [guid]::NewGuid().ToString('N'))
$inputRoot = Join-Path $testRoot 'input'
$appRoot = Join-Path $testRoot 'app'
New-Item -ItemType Directory -Path $inputRoot, $appRoot, $OutputDirectory -Force | Out-Null
$testExe = Join-Path $appRoot 'WinDirStat.exe'
Copy-Item -LiteralPath $Executable -Destination $testExe
# Portable settings and a fixed language make JSON column names predictable.
@('[Options]', 'LanguageId=9', 'AutoElevate=0', 'ShowElevationPrompt=1', 'UseBackupRestore=0', 'UseFastScanEngine=0', 'SampleLargeFiles=0') |
    Set-Content -LiteralPath (Join-Path $appRoot 'WinDirStat.ini') -Encoding utf8

$duplicate = [byte[]]::new(65536)
for ($i = 0; $i -lt $duplicate.Length; $i++) { $duplicate[$i] = [byte]($i % 251) }
[IO.File]::WriteAllBytes((Join-Path $inputRoot 'duplicate-a.bin'), $duplicate)
[IO.File]::WriteAllBytes((Join-Path $inputRoot 'duplicate-b.bin'), $duplicate)
[IO.File]::WriteAllBytes((Join-Path $inputRoot 'large-unique.bin'), [byte[]]::new(1048576))
[IO.File]::WriteAllText((Join-Path $inputRoot '中文样本.txt'), 'fixture-only', [Text.UTF8Encoding]::new($false))
$before = @(Get-ChildItem -LiteralPath $inputRoot -File | Sort-Object Name | ForEach-Object {
    [pscustomobject]@{ Name = $_.Name; Bytes = $_.Length; SHA256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
})

function Invoke-Scan([string]$Flag, [string]$Report) {
    $start = [Diagnostics.ProcessStartInfo]::new($testExe)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WorkingDirectory = $appRoot
    foreach ($argument in @($Flag, $Report, $inputRoot)) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        if (-not $process.WaitForExit(120000)) {
            $process.Kill($true)
            throw "Synthetic scan timed out: $Flag"
        }
        if ($process.ExitCode -ne 0) { throw "Synthetic scan failed: $Flag, exit $($process.ExitCode)" }
        if (-not (Test-Path -LiteralPath $Report)) { throw "Missing report: $Flag" }
    }
    finally { $process.Dispose() }
}

$spaceReport = Join-Path $testRoot 'space.json'
$dupeReport = Join-Path $testRoot 'duplicates.json'
Invoke-Scan '/SaveTo' $spaceReport
Invoke-Scan '/SaveDupesTo' $dupeReport
$space = @(Get-Content -LiteralPath $spaceReport -Raw | ConvertFrom-Json)
$dupes = @(Get-Content -LiteralPath $dupeReport -Raw | ConvertFrom-Json)
foreach ($sample in $before) {
    $row = @($space | Where-Object { $_.Name -eq (Join-Path $inputRoot $sample.Name) })
    if ($row.Count -ne 1 -or $row[0].'Logical Size' -ne $sample.Bytes) {
        throw "Incorrect space report for synthetic file: $($sample.Name)"
    }
}
$rootRow = @($space | Where-Object { $_.Name.TrimEnd('\') -eq $inputRoot.TrimEnd('\') })
if ($rootRow.Count -ne 1 -or $rootRow[0].'Logical Size' -ne 1179660) { throw 'Incorrect logical total.' }
$expectedDupes = @('duplicate-a.bin', 'duplicate-b.bin')
$actualDupes = @($dupes | ForEach-Object { [IO.Path]::GetFileName($_.Name) })
if ($dupes.Count -ne 2 -or @(Compare-Object $expectedDupes $actualDupes).Count -ne 0) {
    throw 'Incorrect duplicate group in synthetic report.'
}
if (@($dupes | Select-Object -ExpandProperty 'Hash Prefix' -Unique).Count -ne 1) { throw 'Duplicates are not grouped together.' }
$after = @(Get-ChildItem -LiteralPath $inputRoot -File | Sort-Object Name | ForEach-Object {
    [pscustomobject]@{ Name = $_.Name; Bytes = $_.Length; SHA256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
})
if (($before | ConvertTo-Json -Compress) -cne ($after | ConvertTo-Json -Compress)) {
    throw 'Synthetic input changed during read-only scans.'
}
# Upload only basename-based synthetic evidence, never absolute runner paths or owner fields.
[ordered]@{
    status = 'passed'
    logical_bytes = 1179660
    largest_file = 'large-unique.bin'
    duplicate_groups = 1
    input_unchanged = $true
    samples = $before
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'synthetic-smoke.json') -Encoding utf8
