# Space observations only. All payloads are generated here; no file deletion or official cleanup.
function Wait-SpaceRecheckIdle {
    param([IntPtr] $Dialog)
    Wait-Until {
        -not [TestDesktop]::IsWindowEnabled((Get-DialogControl $Dialog 'IDC_SR_STOP')) -and
        [TestDesktop]::IsWindowEnabled((Get-DialogControl $Dialog 'IDC_SR_BASELINE'))
    } `
        'The background observation settles' -StableMilliseconds 200 | Out-Null
}
function Set-SpaceTarget {
    param([IntPtr] $Dialog, [string] $Control, [string] $Value)
    $edit = Get-DialogControl $Dialog $Control
    [TestDesktop]::SetText($edit, $Value)
    [void][TestDesktop]::Message($Dialog, 0x0111, ((0x0300 -shl 16) -bor (Get-ResourceId $Control)), $edit.ToInt64())
}
function Test-SpaceRecheck {
    param($Context, $Case)
    $root = $Context.Fixture
    $local = Join-Path $root 'LocalAppData'
    [IO.Directory]::CreateDirectory((Join-Path $local 'nested\deeper')) | Out-Null
    [IO.File]::WriteAllText((Join-Path $root '.windirstat-cleanup-fixture'), 'WinDirStat read-only cleanup fixture v1')
    $payload = Join-Path $local 'payload.bin'
    [IO.File]::WriteAllBytes($payload, [byte[]]::new(4096))
    [IO.File]::WriteAllBytes((Join-Path $local 'nested\sibling.bin'), [byte[]]::new(4096))
    [IO.File]::WriteAllBytes((Join-Path $local 'nested\deeper\leaf.bin'), [byte[]]::new(8192))
    $sentinels = @(Get-ChildItem -LiteralPath (Join-Path $local 'nested') -Recurse -File | ForEach-Object { Get-FileHash -LiteralPath $_.FullName })
    $runner = New-TestRunner @{ Options = @{ LanguageId = $Case.Language; UseSizeSuffixes = 0; AutoElevate = 0 } }
    $runner | Add-Member -NotePropertyName Environment -NotePropertyValue @{ WDS_CLEANUP_TEST_ROOT = $root }
    $session = Start-CleanupSettingsPreview $runner
    $app = $session.App
    $preview = $session.Dialog
    foreach ($entry in 'IDC_CP_WINDOWS_RECOMMENDATIONS','IDC_CP_WINDOWS_DISK') {
        $processes = @('SystemSettings','cleanmgr' | ForEach-Object { [Diagnostics.Process]::GetProcessesByName($_) } | ForEach-Object Id)
        [void][TestDesktop]::PostMessage((Get-DialogControl $preview $entry), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        $dialog = Wait-Until { Get-AppDialog $app 'IDC_SR_BASELINE' } 'The Windows entry opens the observation confirmation'
        Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_SR_RECHECK'))) 'No recheck without a baseline'
        Assert-Equal ([TestDesktop]::Message($dialog, 0x0400).ToInt32() -band 0xFFFF) 2 'Close is default; Enter does not launch a tool'
        Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_DIRECTORY'))) $local 'Only the named synthetic directory is selected'
        Invoke-CleanupButton $dialog 'IDC_SR_BASELINE'
        Wait-SpaceRecheckIdle $dialog
        $baseline = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_RESULT'))
        Assert-True ($baseline -match '16[, .]?384') 'Complete enumeration includes parent and nested siblings exactly once'
        Assert-True ($baseline -match '\r\n(?:Identity|卷身份)') 'Volume identity starts on its own line in the native edit'
        Assert-True ($baseline -match '\r\n(?:Sampling|采样时间)') 'Sampling interval starts on its own line in the native edit'
        Assert-True ($baseline -match 'UTC') 'Sampling time is visible with an explicit time zone'
        Assert-True ($baseline -match 'Volume\{') 'Volume identity is visible with the baseline'
        Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_STATUS')) -match 'simulated|模拟') 'Baseline precedes simulated launch; no official cleanup runs'
        Assert-True ([TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_SR_RECHECK'))) 'Successful baseline enables manual recheck'

        [IO.File]::WriteAllBytes($payload, [byte[]]::new(8192))
        Invoke-CleanupButton $dialog 'IDC_SR_RECHECK'
        Wait-SpaceRecheckIdle $dialog
        $growth = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_RESULT'))
        Assert-True ($growth.Contains($baseline)) 'The original baseline stays intact after recheck'
        Assert-True ($growth -match '\+4[, .]?096') 'Synthetic directory growth has an explicit positive delta'
        Assert-True ($growth -match 'other processes|其他进程') 'Observed changes are not attributed to cleanup'
        Assert-True ($growth -match 'atomic|原子快照') 'The sampling limitation is visible'
        Save-Frame ([TestDesktop]::Capture($dialog)) (Join-Path $Context.Root 'space-recheck.png')

        [IO.File]::WriteAllBytes($payload, [byte[]]::new(0))
        Invoke-CleanupButton $dialog 'IDC_SR_RECHECK'
        Wait-SpaceRecheckIdle $dialog
        $decrease = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_RESULT'))
        Assert-True ($decrease -match '-4[, .]?096') 'Truncating only our generated payload shows a negative directory delta'
        [IO.File]::WriteAllBytes($payload, [byte[]]::new(4096))
        Invoke-CleanupButton $dialog 'IDC_SR_RECHECK'
        Wait-SpaceRecheckIdle $dialog
        $unchanged = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_RESULT'))
        Assert-True ($unchanged -match 'logical 0|逻辑大小0') 'Restoring the payload produces zero logical change'

        Invoke-CleanupButton $dialog 'IDC_SR_RECHECK'
        Invoke-CleanupButton $dialog 'IDC_SR_STOP'
        Wait-SpaceRecheckIdle $dialog
        Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_STATUS')) -match 'cancelled|已取消') 'Cancel publishes no fresh comparison'
        Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_RESULT'))) $unchanged 'Cancel retains the last displayed observation'
        Assert-True ([TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_SR_RECHECK'))) 'Cancel preserves the baseline for retry'

        # Directory replacement is a rename of our fixture, never deletion.
        $moved = $local + '-saved'
        [IO.Directory]::Move($local, $moved)
        Invoke-CleanupButton $dialog 'IDC_SR_RECHECK'
        Wait-SpaceRecheckIdle $dialog
        Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_RESULT')) -match 'incomplete|不完整') 'A missing directory is not reported as zero released space'
        Assert-True ([TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_SR_RECHECK'))) 'Directory read failure allows a retry with the same baseline'
        [IO.Directory]::CreateDirectory($local) | Out-Null
        Invoke-CleanupButton $dialog 'IDC_SR_RECHECK'
        Wait-SpaceRecheckIdle $dialog
        Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_STATUS')) -match 'identity|身份') 'Replacement identity refuses comparison'
        Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_SR_RECHECK'))) 'Replacement requires a new baseline'
        [IO.Directory]::Move($local, $local + '-replacement-' + $entry)
        [IO.Directory]::Move($moved, $local)

        Set-SpaceTarget $dialog 'IDC_SR_DRIVE' 'Z:\'
        Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_SR_RESULT'))) '' 'Editing the target clears stale observations'
        Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_SR_RECHECK'))) 'Editing the target invalidates the baseline'
        Set-SpaceTarget $dialog 'IDC_SR_DRIVE' ([IO.Path]::GetPathRoot($local))
        Invoke-CleanupButton $dialog 'IDC_SR_BASELINE'
        Invoke-CleanupButton $dialog 'IDC_SR_STOP'
        Wait-SpaceRecheckIdle $dialog
        Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_SR_RECHECK'))) 'Cancelled baseline never enables recheck or launch'
        Invoke-CleanupButton $dialog 'IDC_SR_BASELINE'
        Wait-SpaceRecheckIdle $dialog
        Assert-True ([TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_SR_RECHECK'))) 'A cancelled baseline can be retried explicitly'
        Close-AppDialog $dialog
        Wait-Until { [TestDesktop]::IsWindowEnabled($preview) } 'Closing the observation returns to the existing preview' | Out-Null
        $afterProcesses = @('SystemSettings','cleanmgr' | ForEach-Object { [Diagnostics.Process]::GetProcessesByName($_) } | ForEach-Object Id)
        Assert-Equal @($afterProcesses | Where-Object { $_ -notin $processes }).Count 0 'Synthetic observations start neither official Windows tool'
    }
    Assert-Sequence @($sentinels.Path | ForEach-Object { (Get-FileHash -LiteralPath $_).Hash }) @($sentinels.Hash) 'Every nested sentinel is unchanged'
    Close-AppDialog $preview
    Stop-TestApp $app
}
Register-Scenario space.recheck SpaceRecheck 'Space baseline, growth, decrease, cancellation, missing and replaced synthetic directory' `
    Test-SpaceRecheck -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 9 }
Register-Scenario space.recheck.zh SpaceRecheck 'Chinese synthetic space observations without cleanup attribution' `
    Test-SpaceRecheck -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 30724 }
