# Restore only pinned xxHash source files; SourceDirectory enables offline verification.
[CmdletBinding()]
param(
    [string]$LockFile = (Join-Path $PSScriptRoot '..\dependencies\xxhash.json'),
    [string]$Destination = (Join-Path $PSScriptRoot '..\..\contrib\xxhash'),
    [string]$SourceDirectory
)

$ErrorActionPreference = 'Stop'
$lock = Get-Content -LiteralPath $LockFile -Raw | ConvertFrom-Json
$fileNames = @('xxhash.h', 'xxhash.c', 'LICENSE')
if ($lock.repository -ne 'Cyan4973/xxHash' -or $lock.commit -notmatch '^[a-f0-9]{40}$') {
    throw 'Invalid xxHash repository or commit in lock file.'
}
$lockedNames = @($lock.files.PSObject.Properties.Name)
if (@(Compare-Object $fileNames $lockedNames).Count -ne 0) {
    throw 'The lock must contain exactly xxhash.h, xxhash.c and LICENSE.'
}
foreach ($name in $fileNames) {
    if ($lock.files.$name -notmatch '^[a-f0-9]{64}$') { throw "Invalid SHA256 for $name" }
}

New-Item -ItemType Directory -Path $Destination -Force | Out-Null
foreach ($name in $fileNames) {
    $target = Join-Path $Destination $name
    if ($SourceDirectory) {
        Copy-Item -LiteralPath (Join-Path $SourceDirectory $name) -Destination $target
    }
    else {
        $uri = "https://raw.githubusercontent.com/$($lock.repository)/$($lock.commit)/$name"
        Invoke-WebRequest -Uri $uri -OutFile $target
    }
    $actual = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $lock.files.$name) { throw "xxHash SHA256 mismatch: $name" }
    Write-Host "Verified xxHash $($lock.version): $name $actual"
}
