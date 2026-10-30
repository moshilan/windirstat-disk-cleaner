param([Parameter(Mandatory)][string] $Executable, [Parameter(Mandatory)][string] $OutputDirectory)
$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true') { throw 'Actual Explorer verification is restricted to the disposable cloud desktop.' }
$fixture = & $Executable --shell-fixture | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $fixture.target) { throw 'Synthetic folder open request failed.' }
$target = [IO.Path]::GetFullPath($fixture.target)
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
if (-not $target.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $target.Substring($tempRoot.Length) -notmatch '^WinDirStatInstalledAppsShell-[0-9a-f-]+(?: & space\[1\])?\\Copilot\\Application$') {
    throw 'Unexpected folder fixture path; do not interact with other Explorer windows.'
}
$shell = New-Object -ComObject Shell.Application
$deadline = [datetime]::UtcNow.AddSeconds(30)
$observed = $null
try {
    do {
        foreach ($window in @($shell.Windows())) {
            try {
                $path = $window.Document.Folder.Self.Path
                if ([string]::Equals($path, $target, [StringComparison]::OrdinalIgnoreCase)) {
                    $observed = $path
                    # This newly generated exact path identifies our own Explorer window only.
                    $window.Quit()
                    break
                }
            } catch { continue }
        }
        if ($observed) { break }
        Start-Sleep -Milliseconds 100
    } while ([datetime]::UtcNow -lt $deadline)
} finally { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell) }
if (-not $observed) { throw 'Explorer did not enter the exact synthetic Copilot Application directory; parent/Edge fallback is not accepted.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
[ordered]@{
    status = 'PASS'
    expected = $target
    observed = $observed
    same_directory = $true
    parent_or_sibling_accepted = $false
    real_software_directories_opened = 0
    uninstall_calls = 0
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'installed-apps-shell.json') -Encoding utf8
