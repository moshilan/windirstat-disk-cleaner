# Only the fixed BleachBit system.tmp category on a marked, self-created fixture.
function Wait-BleachBitIdle {
    param([IntPtr]$Dialog)
    Wait-Until { -not [TestDesktop]::IsWindowEnabled((Get-DialogControl $Dialog 'IDC_NH_STOP')) } `
        'The external engine worker finishes' -StableMilliseconds 200 | Out-Null
}
function Open-BleachBitFixture {
    param($App, [IntPtr]$Preview)
    [void][TestDesktop]::PostMessage((Get-DialogControl $Preview 'IDC_CP_BLEACHBIT'), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $dialog = Wait-Until { Get-AppDialog $App 'IDC_NH_CLEAR' } 'The separate BleachBit scope dialog opens'
    Wait-BleachBitIdle $dialog
    Assert-True ([TestDesktop]::Text($dialog) -match 'BleachBit') 'The external provider is named explicitly'
    return $dialog
}
function Get-BleachBitConfirmation {
    param($App)
    return Wait-Until {
        @([TestDesktop]::Windows($App.Process.Id) | Where-Object {
            [TestDesktop]::Class($_) -eq '#32770' -and [TestDesktop]::IsWindowVisible($_) -and
            [TestDesktop]::GetDlgItem($_, 1) -ne [IntPtr]::Zero -and [TestDesktop]::GetDlgItem($_, 2) -ne [IntPtr]::Zero
        } | Select-Object -First 1)
    } 'A distinct default-cancel confirmation appears'
}
function Test-BleachBitCategory {
    param($Context, $Case)
    if (-not $env:WDS_TEST_BLEACHBIT) { throw 'A verified cloud test engine is required; no installed-engine fallback.' }
    $root = $Context.Fixture
    $userTemp = Join-Path $root 'LocalAppData\Temp'
    $systemTemp = Join-Path $root 'Windows\Temp'
    foreach ($path in $userTemp, $systemTemp) { [IO.Directory]::CreateDirectory($path) | Out-Null }
    [IO.File]::WriteAllText((Join-Path $root '.windirstat-cleanup-fixture'), 'WinDirStat read-only cleanup fixture v1')
    $userFiles = @(Join-Path $userTemp 'recent.tmp'; Join-Path $userTemp 'recent-arbitrary.data')
    foreach ($path in $userFiles) { [IO.File]::WriteAllText($path, 'self-created recent disposable payload') }
    $sentinels = @(Join-Path $root 'project-sentinel.txt'; Join-Path $systemTemp 'system-protected.tmp')
    foreach ($path in $sentinels) { [IO.File]::WriteAllText($path, 'outside actual cleanup scope, must stay') }
    $before = @($userFiles + $sentinels | ForEach-Object { Get-FileHash -LiteralPath $_ })
    $runner = New-TestRunner @{ Options = @{ LanguageId = $Case.Language; AutoElevate = 0 } }
    $runner | Add-Member -NotePropertyName Environment -NotePropertyValue @{
        WDS_CLEANUP_TEST_ROOT = $root; WDS_BLEACHBIT_TEST_TOOL = $env:WDS_TEST_BLEACHBIT
    }
    $session = Start-CleanupSettingsPreview $runner
    $app = $session.App; $preview = $session.Dialog
    Assert-Equal ([NativeListViewHelper]::GetItemCount((Get-DialogControl $preview 'IDC_CP_FILES'))) 0 `
        'No candidate preview or checked row supplies the external category scope'
    $dialog = Open-BleachBitFixture $app $preview
    Assert-True ([TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_NH_CLEAR'))) 'A complete protected external preview enables explicit confirmation'
    $details = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_NH_DETAILS'))
    Assert-True ($details -match 'LocalAppData\\Temp' -and $details -match 'Windows\\Temp') 'Actual TEMP and protected Windows Temp are displayed'
    Assert-True ($details -match 'system.tmp' -and $details -match '6.0.6') 'Exactly one audited cleaner option and version are shown'
    Assert-True ($details.Contains('recent-arbitrary.data')) 'The real engine preview includes recent non-tmp payloads'
    Assert-True ($details -match '\r\n(?:Version|版本)') 'Scope details use native edit CRLF line breaks'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_NH_NOTICE')) -match 'checkbox|勾选') `
        'The external scope is explicitly independent of candidate selection'
    Assert-Equal ([TestDesktop]::Message($dialog, 0x0400).ToInt32() -band 0xFFFF) 2 'Close is the default, not category cleanup'
    Save-Frame ([TestDesktop]::Capture($dialog)) (Join-Path $Context.Root 'bleachbit-scope.png')
    [void][TestDesktop]::PostMessage((Get-DialogControl $dialog 'IDC_NH_CLEAR'), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $confirm = Get-BleachBitConfirmation $app
    Assert-Equal ([TestDesktop]::Message($confirm, 0x0400).ToInt32() -band 0xFFFF) 2 'The destructive confirmation defaults to Cancel'
    Close-AppDialog $confirm
    foreach ($file in $before) { Assert-Equal (Get-FileHash -LiteralPath $file.Path).Hash $file.Hash 'Cancelling confirmation leaves every payload unchanged' }
    Close-AppDialog $dialog

    # Native tests cover further overlap forms; this proves the actual GUI cannot override one.
    Set-CleanupExclusions $preview $userTemp
    $blocked = Open-BleachBitFixture $app $preview
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $blocked 'IDC_NH_CLEAR'))) 'An exclusion conflict disables the cleaning action'
    [void][TestDesktop]::Message($blocked, 0x0111, (Get-ResourceId 'IDC_NH_CLEAR'), 0)
    Start-Sleep -Milliseconds 100
    Assert-Equal @([TestDesktop]::Windows($app.Process.Id) | Where-Object {
        [TestDesktop]::GetDlgItem($_, 1) -ne [IntPtr]::Zero -and [TestDesktop]::GetDlgItem($_, 2) -ne [IntPtr]::Zero
    }).Count 0 'A direct confirmation command cannot bypass exclusions'
    foreach ($file in $before) { Assert-Equal (Get-FileHash -LiteralPath $file.Path).Hash $file.Hash 'Blocked cleanup preserves all files' }
    Close-AppDialog $blocked
    Set-CleanupExclusions $preview ''

    $dialog = Open-BleachBitFixture $app $preview
    [void][TestDesktop]::PostMessage((Get-DialogControl $dialog 'IDC_NH_CLEAR'), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $confirm = Get-BleachBitConfirmation $app
    Close-AppDialog $confirm 1 # Explicit approval only for these self-created disposable payloads.
    Wait-BleachBitIdle $dialog
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_NH_STATUS')) -match 'completed|执行完成') `
        'A complete real cleanup and follow-up preview reports the engine result'
    foreach ($path in $userFiles) { Assert-True (-not (Test-Path -LiteralPath $path)) 'The real external engine removed an approved synthetic payload' }
    foreach ($path in $sentinels) {
        $hash = $before | Where-Object Path -eq $path
        Assert-Equal (Get-FileHash -LiteralPath $path).Hash $hash.Hash 'Fixed Windows Temp keep and project sentinel survive real cleanup'
    }
    $result = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_NH_DETAILS'))
    Assert-True ($result -match 'Files deleted: 2') 'The original engine output reports actual deleted file count'
    Assert-True ($result -match 'Follow-up preview remaining entry count: 0|清理后预览剩余条目数：0') 'A second real engine preview finds no eligible synthetic payloads'
    Assert-True ($result -match 'volume observation|磁盘观测') 'Volume change is not presented as attributed release'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_NH_CLEAR'))) 'A completed scope cannot be replayed silently'
    Save-Frame ([TestDesktop]::Capture($dialog)) (Join-Path $Context.Root 'bleachbit-result.png')
    Close-AppDialog $dialog
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $preview 'IDC_CP_STATUS')) -match 'stale|失效') 'Started external cleanup invalidates the old candidate preview'
    Close-AppDialog $preview
    Stop-TestApp $app
}

Register-Scenario cleanup.bleachbit CleanupPreview 'BleachBit protected user Temp preview, cancel, exclusion and real cleanup' `
    Test-BleachBitCategory -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 9 }
Register-Scenario cleanup.bleachbit.zh CleanupPreview 'Chinese BleachBit protected Temp confirmation and real synthetic cleanup' `
    Test-BleachBitCategory -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 30724 }
