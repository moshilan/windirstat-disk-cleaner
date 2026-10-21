function Get-ScanPolicyTabs {
    param($App)
    foreach ($tab in [TestDesktop]::Children($App.Window)) {
        if ([TestDesktop]::Class($tab) -eq 'SysTabControl32') {
            [NativeListViewHelper]::GetTabTexts($tab) | ForEach-Object { $_.Trim() }
        }
    }
}

function Open-SyntheticScanSelection {
    param($App, [string] $Root)
    $dialog = Open-AppDialog $App 'ID_FILE_SELECT' 'IDC_SCAN_DUPLICATES'
    Assert-Equal ([TestDesktop]::Message((Get-DialogControl $dialog 'IDC_RADIO_TARGET_FOLDER'), 0x00F0).ToInt32()) 1 `
        'The selected target is a folder, never a real volume'
    Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_BROWSE_FOLDER'))) $Root `
        'The only accepted target is the synthetic fixture'
    Assert-Equal ([TestDesktop]::Message((Get-DialogControl $dialog 'IDC_SCAN_DUPLICATES'), 0x00F0).ToInt32()) 0 `
        'Each new scan requires a fresh duplicate-hashing opt-in'
    return $dialog
}

function Test-ScanPolicy {
    param($Context, $Case)
    $root = New-TestTree
    $before = @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | Get-FileHash)
    $runner = New-TestRunner @{
        DupeView = @{ ScanForDuplicates = 1 }
        DriveSelect = @{ SelectDrivesRadio = 2; SelectDrivesFolder = $root }
    }
    $app = Start-TestApp @($root) -Runner $runner
    Get-AppList $app -AllFiles | Out-Null
    Assert-True ('Duplicate Files' -notin @(Get-ScanPolicyTabs $app)) `
        'A stale persisted opt-in does not enable hashing on ordinary startup'
    Save-Frame ([TestDesktop]::Capture($app.Window)) (Join-Path $Context.Root 'scan-complete.png')
    $normal = Save-AppReport $app 'normal-before' -Format json
    Assert-ScanSnapshot $normal (Get-DiskSnapshot $root) $root

    $dialog = Open-SyntheticScanSelection $app $root
    Set-DialogCheck $dialog 'IDC_SCAN_DUPLICATES' $true
    Close-AppDialog $dialog 1
    Wait-AppIdle $app
    Invoke-AppCommand $app 'ID_VIEW_DUPLICATE_FILES'
    $dupes = Save-AppReport $app 'explicit-duplicates' 'ID_SAVE_DUPLICATES' json
    Assert-Sequence @($dupes.Name | ForEach-Object { [IO.Path]::GetFileName($_) } | Sort-Object) `
        @('first.wdsdupe', 'second.wdsdupe') 'Explicit opt-in retains accurate duplicate detection'

    $dialog = Open-SyntheticScanSelection $app $root
    Close-AppDialog $dialog
    Assert-True ('Duplicate Files' -in @(Get-ScanPolicyTabs $app)) `
        'Cancelling a new selection preserves the current duplicate results'
    $dialog = Open-SyntheticScanSelection $app $root
    Close-AppDialog $dialog 1
    Wait-AppIdle $app
    Get-AppList $app -AllFiles | Out-Null
    Assert-True ('Duplicate Files' -notin @(Get-ScanPolicyTabs $app)) `
        'The next ordinary scan does not inherit the previous explicit opt-in'
    Assert-ScanSnapshot (Save-AppReport $app 'normal-after' -Format json) (Get-DiskSnapshot $root) $root
    Stop-TestApp $app
    $explicit = Start-TestApp @($root) -ScanDuplicates
    Assert-True ('Duplicate Files' -in @(Get-ScanPolicyTabs $explicit)) `
        'The migrated test helper starts hashing through an explicit folder selection'
    Assert-Sequence @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | Get-FileHash |
        ForEach-Object Hash) @($before.Hash) 'Policy tests leave all synthetic file contents unchanged'
}

function Test-ScanWorkloads {
    param($Context, $Case)
    $root = $Context.Fixture
    # Same size and first MiB force full-content verification; only the first pair is equal.
    $payload = [byte[]]::new(2MB)
    for ($index = 0; $index -lt 64; ++$index) {
        [BitConverter]::GetBytes([int][Math]::Max(0, $index - 1)).CopyTo($payload, 1MB + 128)
        [IO.File]::WriteAllBytes((Join-Path $root ('sample-{0:d2}.bin' -f $index)), $payload)
    }
    $before = @(Get-ChildItem -LiteralPath $root -File | Sort-Object Name | Get-FileHash)
    $runner = New-TestRunner @{ Options = @{ SampleLargeFiles = 0 } }
    $measurements = @()
    foreach ($round in 1..2) {
        foreach ($kind in 'Scan', 'Duplicates') {
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $rows = Invoke-ScanReport $runner @($root) "workload-$kind-$round" -Format json -Kind $kind
            $watch.Stop()
            if ($kind -eq 'Scan') {
                $rootRow = @($rows | Where-Object { $_.Name.TrimEnd('\') -eq $root.TrimEnd('\') })
                Assert-Equal ([long]$rootRow[0].'Logical Size') ([long]128MB) 'Space workload preserves byte totals'
            } else {
                Assert-Sequence @($rows.Name | ForEach-Object { [IO.Path]::GetFileName($_) } | Sort-Object) `
                    @('sample-00.bin', 'sample-01.bin') 'Full hashing rejects same-prefix files with different contents'
                Assert-Equal @($rows | Where-Object { [string]$_.'Sampled Hash' -ine 'false' }).Count 0 `
                    'Explicit duplicate scans retain full-content accuracy'
            }
            $measurements += [ordered]@{ mode = $kind; round = $round; seconds = [Math]::Round($watch.Elapsed.TotalSeconds, 3) }
        }
    }
    Assert-Sequence @(Get-ChildItem -LiteralPath $root -File | Sort-Object Name | Get-FileHash |
        ForEach-Object Hash) @($before.Hash) 'Workload comparison leaves all source payloads unchanged'
    # Only counts and timings are emitted; raw scan reports and owner/path fields stay in runner temp.
    Write-TestNote ('Synthetic workload: ' + (@{ files = 64; logical_bytes = 128MB; measurements = $measurements } |
        ConvertTo-Json -Depth 5 -Compress))
}

Register-Scenario scan.policy ScanPolicy 'Duplicate hashing is an explicit choice for each new scan' `
    Test-ScanPolicy -Tags Desktop -Requires Windows,Desktop
Register-Scenario scan.workloads ScanPolicy 'Compare enumeration and full duplicate hashing on synthetic files' `
    Test-ScanWorkloads -Requires Windows
