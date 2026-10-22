function New-CleanupFixtureFile {
    param([string] $Path, [int] $Size = 4097, [switch] $Recent)
    New-TestFile $Path $Size | Out-Null
    $time = if ($Recent) { [datetime]::UtcNow } else { [datetime]::UtcNow.AddDays(-10) }
    [IO.File]::SetCreationTimeUtc($Path, $time)
    [IO.File]::SetLastWriteTimeUtc($Path, $time)
    return $Path
}

function Invoke-CleanupButton {
    param([IntPtr] $Dialog, [string] $Name)
    $button = Get-DialogControl $Dialog $Name
    Assert-True ([TestDesktop]::IsWindowEnabled($button)) "Cleanup control $Name is enabled"
    [TestDesktop]::Activate($Dialog)
    [void] [TestDesktop]::Message($button, 0x00F5)
}

function Wait-CleanupPreviewIdle {
    param([IntPtr] $Dialog)
    $scan = Get-DialogControl $Dialog 'IDC_CP_SCAN'
    $cancel = Get-DialogControl $Dialog 'IDC_CP_CANCEL'
    Wait-Until {
        [TestDesktop]::IsWindowEnabled($scan) -and -not [TestDesktop]::IsWindowEnabled($cancel)
    } 'The cleanup worker and preview updates settle' -StableMilliseconds 350 | Out-Null
}

function Set-CleanupExclusions {
    param([IntPtr] $Dialog, [string] $Value)
    $edit = Get-DialogControl $Dialog 'IDC_CP_EXCLUSIONS'
    [TestDesktop]::SetText($edit, $Value)
    # Simulate user editing: multiline WM_SETTEXT itself does not send EN_CHANGE.
    [void] [TestDesktop]::Message($Dialog, 0x0111,
        ((0x0300 -shl 16) -bor (Get-ResourceId 'IDC_CP_EXCLUSIONS')), $edit.ToInt64())
}

function Test-CleanupPreview {
    param($Context, $Case)
    $root = $Context.Fixture
    $local = Join-Path $root 'LocalAppData'
    [IO.Directory]::CreateDirectory($local) | Out-Null
    [IO.File]::WriteAllText((Join-Path $root '.windirstat-cleanup-fixture'), 'WinDirStat read-only cleanup fixture v1')
    $relative = @(
        'Temp\old.tmp', 'Temp\excluded\item.tmp', 'Temp\excluded-sibling\item.temp',
        'Google\Chrome\User Data\Default\Cache\Cache_Data\entry.bin',
        'Microsoft\Edge\User Data\Profile 1\Cache\Cache_Data\entry.bin',
        'Mozilla\Firefox\Profiles\sample.default\cache2\entries\0123456789ABCDEF0123456789ABCDEF01234567',
        'pip\Cache\http-v2\a\b\c\d\e\response.body'
    )
    $expected = @($relative | ForEach-Object { New-CleanupFixtureFile (Join-Path $local $_) })
    New-CleanupFixtureFile (Join-Path $local 'Temp\recent.tmp') -Recent | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Temp\personal.docx') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Google\Chrome\User Data\Default\Cookies') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Google\Chrome\User Data\Default\Service Worker\CacheStorage\personal') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'pip\Cache\wheels\package.whl') | Out-Null
    $before = @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | Get-FileHash)
    $runner = New-TestRunner @{ Options = @{ LanguageId = $Case.Language } }
    $runner | Add-Member -NotePropertyName Environment -NotePropertyValue @{ WDS_CLEANUP_TEST_ROOT = $root }
    $app = Start-TestApp -Runner $runner
    $selection = Wait-Until { Get-AppDialog $app 'IDC_SCAN_DUPLICATES' } 'Initial target selection is cancelled without scanning any disk'
    Close-AppDialog $selection
    Wait-AppIdle $app
    $menu = Find-MenuCommand ([TestDesktop]::GetMenu($app.Window)) (Get-ResourceId 'ID_CLEANUP_PREVIEW')
    Assert-True ($null -ne $menu) 'Cleanup preview is present without an existing disk model'
    Invoke-AppCommand $app 'ID_CLEANUP_PREVIEW' -Unchecked
    $dialog = Wait-Until { Get-AppDialog $app 'IDC_CP_SCAN' } 'The independent cleanup preview opens'
    $rectangle = [TestDesktop]::Rectangle($dialog)
    $work = [System.Windows.Forms.Screen]::FromHandle($dialog).WorkingArea
    Assert-True ($rectangle[2] -gt $rectangle[0] -and $rectangle[3] -gt $rectangle[1] -and
        $rectangle[0] -ge $work.Left -and $rectangle[1] -ge $work.Top -and
        $rectangle[2] -le $work.Right -and $rectangle[3] -le $work.Bottom) `
        'The native preview has a valid size and fits entirely in the working area'
    $files = Get-DialogControl $dialog 'IDC_CP_FILES'
    $categories = Get-DialogControl $dialog 'IDC_CP_CATEGORIES'
    $notice = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_NOTICE'))
    Assert-True ($notice -match 'synthetic|合成') 'The fixture override is explicitly visible'
    Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) 0 'Opening the preview does not start a scan'
    Assert-Equal ([NativeListViewHelper]::GetItemCount($categories)) 5 'Five audited categories are available'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_CP_CANCEL'))) 'Idle preview has no running worker'
    $buttons = @([TestDesktop]::Children($dialog) | Where-Object { [TestDesktop]::Class($_) -eq 'Button' } |
        ForEach-Object { [TestDesktop]::ControlText($_) })
    Assert-True (-not @($buttons | Where-Object { $_ -match '删除|执行清理|Delete|Execute|Clean now' }).Count) `
        'The preview has no deletion or cleanup execution button'

    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $dialog
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($files) | Sort-Object) @($expected | Sort-Object) `
        'Only audited old temporary and cache files appear in the GUI'
    $counts = @([NativeListViewHelper]::GetItemTexts($categories, 1))
    Assert-Sequence $counts @('3','1','1','1','1') 'Category counts assign each candidate once'
    foreach ($column in 1,2,3,4,5) {
        $values = @([NativeListViewHelper]::GetItemTexts($files, $column))
        Assert-Equal @($values | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count 0 `
            "Candidate column $column is populated with category, allocation, rule, reason or risk"
    }
    for ($index = 0; $index -lt $expected.Count; ++$index) {
        Assert-Equal ([TestDesktop]::Message($files, 0x102C, $index, 0xF000).ToInt32() -band 0xF000) 0x1000 `
            'Every candidate starts unchecked'
    }
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_ALL'
    for ($index = 0; $index -lt $expected.Count; ++$index) {
        Assert-Equal ([TestDesktop]::Message($files, 0x102C, $index, 0xF000).ToInt32() -band 0xF000) 0x2000 `
            'Explicit bulk selection checks candidates without modifying them'
    }
    $allTotals = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_TOTALS'))
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_NONE'
    $noneTotals = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_TOTALS'))
    Assert-True ($allTotals -ne $noneTotals) 'Selected counts and allocation estimates update with selection'
    Assert-True ([NativeListViewHelper]::FocusListView($files) -and [NativeListViewHelper]::SelectSingleItem($files, 0)) `
        'A candidate can be focused for complete details'
    $first = [NativeListViewHelper]::GetItemTexts($files)[0]
    Wait-Until { [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_DETAILS')).Contains($first) } `
        'Full candidate path is available outside clipped columns' | Out-Null
    Save-Frame ([TestDesktop]::Capture($dialog)) (Join-Path $Context.Root 'cleanup-preview.png')

    $excluded = Join-Path $local 'Temp\excluded'
    Set-CleanupExclusions $dialog $excluded
    Wait-Until { [NativeListViewHelper]::GetItemCount($files) -eq 0 } 'Changing exclusions invalidates the old preview and selection' | Out-Null
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $dialog
    $afterExclusion = @([NativeListViewHelper]::GetItemTexts($files))
    Assert-Equal $afterExclusion.Count 6 'Excluding one directory removes exactly one candidate'
    Assert-True ((Join-Path $local 'Temp\excluded-sibling\item.temp') -in $afterExclusion) `
        'A similarly named sibling is not accidentally excluded'
    Assert-True ((Join-Path $local 'Temp\excluded\item.tmp') -notin $afterExclusion) 'The excluded file is absent'
    Assert-Sequence @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | Get-FileHash | ForEach-Object Hash) `
        @($before.Hash) 'Repeated previews and selections leave every source payload unchanged'
    Set-CleanupExclusions $dialog 'relative-directory'
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_STATUS')) -match 'absolute|绝对') `
        'An invalid exclusion is explained without starting a scan'
    Set-CleanupExclusions $dialog $local
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $dialog
    Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) 0 'Excluding the entire anchor produces an explicit empty preview'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_STATUS')) -match 'no candidates|未发现') `
        'A genuinely empty preview is distinct from cancellation and failure'
    Set-CleanupExclusions $dialog $excluded

    # ACL changes are confined to this new synthetic directory and always restored.
    $blocked = Join-Path $local 'Temp\blocked'
    New-CleanupFixtureFile (Join-Path $blocked 'private.tmp') | Out-Null
    $acl = Get-Acl -LiteralPath $blocked
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    try { $sid = $identity.User } finally { $identity.Dispose() }
    $denied = Get-Acl -LiteralPath $blocked
    $denied.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,
        [Security.AccessControl.FileSystemRights]::ListDirectory, [Security.AccessControl.AccessControlType]::Deny))
    Add-Cleanup 'Restore synthetic cleanup directory access' {
        param($Directory, $Security); Set-Acl -LiteralPath $Directory -AclObject $Security
    } @($blocked, $acl)
    Set-Acl -LiteralPath $blocked -AclObject $denied
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $dialog
    $issues = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_ISSUES'))
    Assert-True ($issues.Contains($blocked)) 'An unreadable location is explained with its path'
    Assert-True ($issues.StartsWith($blocked)) 'Actual failure details appear before general estimate notes'
    $state = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_STATUS'))
    Assert-True ($state -match 'incomplete|不完整') 'Read failures do not report a complete preview'
    Set-Acl -LiteralPath $blocked -AclObject $acl
    Assert-Equal (Get-Acl -LiteralPath $blocked).Sddl $acl.Sddl 'Synthetic directory permissions are restored exactly'

    # Zero-byte metadata workload permits reliable cancellation without large disk writes.
    for ($index = 0; $index -lt 2000; ++$index) {
        New-CleanupFixtureFile (Join-Path $local "Temp\stress\file-$index.tmp") 0 | Out-Null
    }
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Invoke-CleanupButton $dialog 'IDC_CP_CANCEL'
    Wait-CleanupPreviewIdle $dialog
    $state = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_STATUS'))
    Assert-True ($state -match 'cancel|取消') 'Cancellation is explicit and distinct from completion'
    foreach ($item in $before) {
        Assert-Equal (Get-FileHash -LiteralPath $item.Path).Hash $item.Hash 'Cancellation leaves original source payloads unchanged'
    }
    Close-AppDialog $dialog
    Wait-AppIdle $app
    Stop-TestApp $app

    $invalid = Join-Path $root 'invalid-override'
    [IO.Directory]::CreateDirectory((Join-Path $invalid 'LocalAppData')) | Out-Null
    [IO.File]::WriteAllText((Join-Path $invalid '.windirstat-cleanup-fixture'), 'invalid')
    $invalidRunner = New-TestRunner @{ Options = @{ LanguageId = $Case.Language } }
    $invalidRunner | Add-Member -NotePropertyName Environment -NotePropertyValue @{ WDS_CLEANUP_TEST_ROOT = $invalid }
    $invalidApp = Start-TestApp -Runner $invalidRunner
    $selection = Wait-Until { Get-AppDialog $invalidApp 'IDC_SCAN_DUPLICATES' } 'Invalid override startup still performs no scan'
    Close-AppDialog $selection
    Invoke-AppCommand $invalidApp 'ID_CLEANUP_PREVIEW' -Unchecked
    $invalidDialog = Wait-Until { Get-AppDialog $invalidApp 'IDC_CP_SCAN' } 'Invalid fixture is handled within the preview'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $invalidDialog 'IDC_CP_SCAN'))) `
        'Invalid synthetic override disables scanning instead of falling back to real user caches'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $invalidDialog 'IDC_CP_EXCLUSIONS'))) `
        'Unavailable roots keep configuration disabled and preserve the failure explanation'
    Assert-Equal ([NativeListViewHelper]::GetItemCount((Get-DialogControl $invalidDialog 'IDC_CP_FILES'))) 0 `
        'An invalid override never creates candidates'
    Close-AppDialog $invalidDialog
    Stop-TestApp $invalidApp
}

Register-Scenario cleanup.preview CleanupPreview 'Read-only cleanup categories, selection, exclusions and cancellation' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 9 }
Register-Scenario cleanup.preview.zh CleanupPreview 'Chinese read-only cleanup preview and failure explanations' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 30724 }
