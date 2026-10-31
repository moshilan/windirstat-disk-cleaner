# Only the explicitly selected bleachbit-temp cloud suite uses this pinned external test engine.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or -not $env:RUNNER_TEMP) {
    throw 'This download is restricted to the explicitly authorized cloud synthetic test suite.'
}
$lock = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../dependencies/bleachbit-test.json') -Raw | ConvertFrom-Json
if ($lock.version -ne '6.0.6' -or $lock.sha256 -notmatch '^[0-9a-f]{64}$' -or
    $lock.url -ne 'https://github.com/bleachbit/bleachbit/releases/download/v6.0.6/BleachBit-6.0.6-portable.zip') {
    throw 'Unexpected BleachBit test dependency.'
}
$testRoot = Join-Path $env:RUNNER_TEMP ('WinDirStatBleachBitEngine-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$archivePath = Join-Path $testRoot 'official-portable.zip'
Invoke-WebRequest -Uri $lock.url -OutFile $archivePath
$hash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($hash -ne $lock.sha256 -or (Get-Item -LiteralPath $archivePath).Length -ne $lock.bytes) {
    throw 'BleachBit test archive digest or length mismatch.'
}
$destination = Join-Path $testRoot 'engine'
[IO.Directory]::CreateDirectory($destination) | Out-Null
$destinationRoot = [IO.Path]::GetFullPath($destination) + [IO.Path]::DirectorySeparatorChar
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    foreach ($entry in $zip.Entries) {
        $target = [IO.Path]::GetFullPath((Join-Path $destination $entry.FullName))
        if (-not $target.StartsWith($destinationRoot, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Unsafe test archive entry.'
        }
    }
} finally { $zip.Dispose() }
[IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $destination)
$tools = @(Get-ChildItem -LiteralPath $destination -Recurse -File -Filter bleachbit_console.exe)
if ($tools.Count -ne 1) { throw 'Expected exactly one external console executable in the complete official package.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
[ordered]@{ version = $lock.version; source_commit = $lock.source_commit; package_sha256 = $hash;
    package_bytes = $lock.bytes; installed = $false; bundled_in_product = $false } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'bleachbit-test-engine.json') -Encoding utf8
$env:WDS_TEST_BLEACHBIT = $tools[0].FullName
"WDS_TEST_BLEACHBIT=$env:WDS_TEST_BLEACHBIT" >> $env:GITHUB_ENV
Write-Host "Verified external BleachBit $($lock.version) package for cloud synthetic tests only."
