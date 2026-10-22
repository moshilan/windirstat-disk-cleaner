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

function Get-CleanupAccessRules {
    param($Acl)
    return @($Acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier]) | ForEach-Object {
        '{0}|{1}|{2}|{3}|{4}|{5}' -f $_.IdentityReference.Value, [long]$_.FileSystemRights,
            $_.AccessControlType, $_.InheritanceFlags, $_.PropagationFlags, $_.IsInherited
    } | Sort-Object)
}

function Get-CleanupTotalsWithoutVisible {
    param([IntPtr] $Dialog)
    return ([TestDesktop]::ControlText((Get-DialogControl $Dialog 'IDC_CP_TOTALS')) -replace '\s*\|\s*(Visible:|显示：).*$', '')
}

function Move-CleanupDivider {
    param([IntPtr] $Dialog, [IntPtr] $Above, [IntPtr] $Below, [int] $Offset, [IntPtr] $GapBelow = $Below)
    $upper = [TestDesktop]::Rectangle($Above)
    $lower = [TestDesktop]::Rectangle($Below)
    $gapEnd = [TestDesktop]::Rectangle($GapBelow)
    $window = [TestDesktop]::Rectangle($Dialog)
    $client = [TestDesktop]::ClientSize($Dialog)
    # A restored thick-frame dialog has equal left/right/bottom frame widths.
    $border = [int](($window[2] - $window[0] - $client[0]) / 2)
    $originX = $window[0] + $border
    $originY = $window[3] - $border - $client[1]
    $x = [int](($upper[0] + $upper[2]) / 2) - $originX
    $y = [int](($upper[3] + $gapEnd[1]) / 2) - $originY
    $point = ($y -shl 16) -bor ($x -band 0xFFFF)
    [TestDesktop]::Activate($Dialog)
    # Native capture also delivers real cursor moves. Keep the cursor and the
    # injected button coordinates together instead of racing two positions.
    [TestDesktop]::MoveCursor($originX + $x, $originY + $y)
    Start-Sleep -Milliseconds 100
    Assert-Equal ([TestDesktop]::WindowAt($originX + $x, $originY + $y)) $Dialog 'A divider is reachable by native pointer hit testing'
    [TestDesktop]::LeftButton($true)
    try {
        Wait-Until { [TestDesktop]::CaptureOwner($Dialog) -eq $Dialog } 'A native divider press acquires mouse capture' | Out-Null
        [TestDesktop]::MoveCursor($originX + $x, $originY + $y + $Offset)
        Wait-Until { ([TestDesktop]::Rectangle($Above)[3] - $upper[3]) -eq $Offset } `
            'The captured native cursor move settles at the requested divider position' -StableMilliseconds 150 | Out-Null
    } finally { [TestDesktop]::LeftButton($false) }
    Wait-Until { [TestDesktop]::CaptureOwner($Dialog) -eq [IntPtr]::Zero } 'Releasing the native divider press releases capture' | Out-Null
    $movedUpper = [TestDesktop]::Rectangle($Above)
    $movedLower = [TestDesktop]::Rectangle($Below)
    Assert-Equal ($movedUpper[3] - $upper[3]) $Offset 'The upper preview region follows the divider'
    Assert-Equal ($movedLower[1] - $lower[1]) $Offset 'The lower preview region follows the divider'
    Assert-True ($movedUpper[3] -lt $movedLower[1]) 'Dragging preserves a gap between preview regions'
    Assert-Equal ([TestDesktop]::CaptureOwner($Dialog)) ([IntPtr]::Zero) 'Finishing a divider drag releases capture'
}

function Click-CleanupFilter {
    param([IntPtr] $Dialog, [bool] $Expected, [switch] $Label)
    $button = Get-DialogControl $Dialog 'IDC_CP_HIDE_ZERO'
    $rect = [TestDesktop]::Rectangle($button)
    [TestDesktop]::Activate($Dialog)
    $x = $rect[0] + $(if ($Label) { 60 } else { 8 })
    $y = [int](($rect[1] + $rect[3]) / 2)
    [TestDesktop]::MoveCursor($x, $y)
    Assert-Equal ([TestDesktop]::WindowAt($x, $y)) $button 'The native filter pointer reaches the checkbox or its label'
    [TestDesktop]::LeftButton($true)
    Start-Sleep -Milliseconds 80
    [TestDesktop]::LeftButton($false)
    Wait-Until { [TestDesktop]::Message($button, 0x00F0).ToInt32() -eq [int]$Expected } `
        'A native filter click reaches the requested checked state' -StableMilliseconds 150 | Out-Null
}

function Resize-CleanupByPointer {
    param([IntPtr] $Dialog)
    $rect = [TestDesktop]::Rectangle($Dialog)
    $minimum = [TestDesktop]::MinimumTrackSize($Dialog)
    Assert-True ($minimum[0] -lt $rect[2] - $rect[0] -and $minimum[1] -lt $rect[3] - $rect[1]) `
        "Native minimum size $($minimum -join 'x') leaves shrink room inside $($rect[2] - $rect[0])x$($rect[3] - $rect[1])"
    $midX = [int](($rect[0] + $rect[2]) / 2)
    $midY = [int](($rect[1] + $rect[3]) / 2)
    $edges = @(
        @(($rect[0] + 3), $midY, 12, 0, 10), @(($rect[2] - 3), $midY, -12, 0, 11),
        @($midX, ($rect[1] + 3), 0, 12, 12), @($midX, ($rect[3] - 3), 0, -12, 15),
        @(($rect[0] + 3), ($rect[1] + 3), 12, 12, 13), @(($rect[2] - 3), ($rect[1] + 3), -12, 12, 14),
        @(($rect[0] + 3), ($rect[3] - 3), 12, -12, 16), @(($rect[2] - 3), ($rect[3] - 3), -12, -12, 17)
    )
    foreach ($edge in $edges) {
        $x, $y, $dx, $dy, $hit = $edge
        $point = ($y -shl 16) -bor ($x -band 0xFFFF)
        Assert-Equal ([TestDesktop]::Message($Dialog, 0x0084, 0, $point).ToInt32()) $hit `
            'Each window edge and corner exposes its native sizing hit region'
        [TestDesktop]::Activate($Dialog)
        [TestDesktop]::MoveCursor($x, $y)
        Assert-Equal ([TestDesktop]::WindowAt($x, $y)) $Dialog 'The native sizing pointer really reaches the preview window'
        [TestDesktop]::LeftButton($true)
        try {
            Wait-Until { [TestDesktop]::CaptureOwner($Dialog) -eq $Dialog } `
                'A native frame press enters the system sizing loop' -Seconds 5 | Out-Null
            Start-Sleep -Milliseconds 150
            [TestDesktop]::MoveCursor($x + $dx, $y + $dy)
            Start-Sleep -Milliseconds 150
        } finally { [TestDesktop]::LeftButton($false) }
        Wait-Until { [TestDesktop]::CaptureOwner($Dialog) -eq [IntPtr]::Zero } 'The native sizing loop ends after release' | Out-Null
        # With outline-only native sizing, the rectangle changes on release.
        Wait-Until {
            $current = [TestDesktop]::Rectangle($Dialog)
            ($dx -eq 0 -or $current[2] - $current[0] -lt $rect[2] - $rect[0]) -and
                ($dy -eq 0 -or $current[3] - $current[1] -lt $rect[3] - $rect[1])
        } 'A real edge or corner drag can shrink the initial preview' -Seconds 10 -StableMilliseconds 150 | Out-Null
        [TestDesktop]::Position($Dialog, $rect[0], $rect[1], $rect[2] - $rect[0], $rect[3] - $rect[1])
    }
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
    $largest = New-CleanupFixtureFile (Join-Path $local 'Temp\old.tmp') 32769
    $zero = New-CleanupFixtureFile (Join-Path $local 'Temp\zero.tmp') 0
    $expected += $zero
    New-CleanupFixtureFile (Join-Path $local 'Temp\recent.tmp') -Recent | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Temp\personal.docx') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Google\Chrome\User Data\Default\Cookies') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Google\Chrome\User Data\Default\Service Worker\CacheStorage\personal') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'pip\Cache\wheels\package.whl') | Out-Null
    $before = @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | Get-FileHash)
    $desktop = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $fontSize = if ($Case.FontSize -eq 200 -and ($desktop.Width -lt 2000 -or $desktop.Height -lt 1600)) { 100 } else { $Case.FontSize }
    Write-TestNote "Native resize desktop: $($desktop.Width)x$($desktop.Height); font=$fontSize%; full-window-drag=$([System.Windows.Forms.SystemInformation]::DragFullWindows)"
    $runner = New-TestRunner @{ Options = @{ LanguageId = $Case.Language; FontSizePercent = $fontSize; DarkMode = 1 } }
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
    Resize-CleanupByPointer $dialog
    $initialFiles = [TestDesktop]::Rectangle($files)
    $width = $rectangle[2] - $rectangle[0]
    $height = $rectangle[3] - $rectangle[1]
    [TestDesktop]::Position($dialog, $work.Left + 4, $work.Top + 4, $width + 8, $height + 8)
    $resizedFiles = [TestDesktop]::Rectangle($files)
    Assert-Equal ($resizedFiles[2] - $resizedFiles[0] - $initialFiles[2] + $initialFiles[0]) 8 `
        'The file list grows with the preview width'
    Assert-Equal ($resizedFiles[3] - $resizedFiles[1] - $initialFiles[3] + $initialFiles[1]) 8 `
        'The file list grows with the preview height'
    [TestDesktop]::Position($dialog, $rectangle[0], $rectangle[1], $width, $height)
    $columnWidth = [TestDesktop]::Message($files, 0x101D, 0).ToInt32()
    Assert-True ([NativeListViewHelper]::SetHeaderWidth($files, 0, $columnWidth + 70)) `
        'The native path column accepts a user header resize'
    Assert-Equal ([TestDesktop]::Message($files, 0x101D, 0).ToInt32()) ($columnWidth + 70) `
        'The resized path column keeps its requested width'
    Move-CleanupDivider $dialog $categories $files 12 -GapBelow (Get-DialogControl $dialog 'IDC_CP_SORT_SIZE')
    Move-CleanupDivider $dialog $files (Get-DialogControl $dialog 'IDC_CP_DETAILS') -12
    $exclusionsControl = Get-DialogControl $dialog 'IDC_CP_EXCLUSIONS'
    $issuesControl = Get-DialogControl $dialog 'IDC_CP_ISSUES'
    $logHeight = [TestDesktop]::Rectangle($issuesControl)[3] - [TestDesktop]::Rectangle($issuesControl)[1]
    Move-CleanupDivider $dialog $exclusionsControl $issuesControl -24
    Assert-Equal ([TestDesktop]::Rectangle($issuesControl)[3] - [TestDesktop]::Rectangle($issuesControl)[1]) ($logHeight + 24) `
        'Dragging the log upper boundary increases the visible diagnostic height'
    Move-CleanupDivider $dialog $exclusionsControl $issuesControl 24
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
    Assert-Sequence $counts @('4','1','1','1','1') 'Category counts include zero-allocation candidates exactly once'
    $coverage = @([NativeListViewHelper]::GetItemTexts($categories, 4))
    Assert-Equal @($coverage | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count 0 `
        'All five categories explain their discovery outcome'
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

    # One checked identity must survive numeric sorting rather than following its old row index.
    $largestRow = [array]::IndexOf([NativeListViewHelper]::GetItemTexts($files), $largest)
    Assert-True ([NativeListViewHelper]::FocusListView($files) -and [NativeListViewHelper]::SelectSingleItem($files, $largestRow)) `
        'The largest candidate is focused before checking it'
    [void] [TestDesktop]::Message($files, 0x0100, 0x20)
    [void] [TestDesktop]::Message($files, 0x0101, 0x20)
    Assert-Equal ([TestDesktop]::Message($files, 0x102C, $largestRow, 0xF000).ToInt32() -band 0xF000) 0x2000 `
        'Keyboard selection checks the intended candidate'
    $selectedTotals = Get-CleanupTotalsWithoutVisible $dialog
    Invoke-CleanupButton $dialog 'IDC_CP_SORT_SIZE'
    Assert-Equal ([NativeListViewHelper]::GetItemTexts($files)[0]) $largest 'Size descending puts the largest allocation first'
    Assert-Equal ([TestDesktop]::Message($files, 0x102C, 0, 0xF000).ToInt32() -band 0xF000) 0x2000 `
        'The same candidate stays checked after descending sorting'
    Assert-Equal (Get-CleanupTotalsWithoutVisible $dialog) $selectedTotals 'Sorting does not change candidate or selected totals'
    Invoke-CleanupButton $dialog 'IDC_CP_SORT_SIZE'
    Assert-Equal ([NativeListViewHelper]::GetItemTexts($files)[0]) $zero 'Size ascending puts the zero-allocation candidate first'
    $largestRow = [array]::IndexOf([NativeListViewHelper]::GetItemTexts($files), $largest)
    Assert-Equal ([TestDesktop]::Message($files, 0x102C, $largestRow, 0xF000).ToInt32() -band 0xF000) 0x2000 `
        'Ascending sorting preserves the checked file identity'
    Assert-True ([NativeListViewHelper]::FocusListView($files) -and [NativeListViewHelper]::SelectSingleItem($files, 0)) `
        'A sorted candidate can be selected for details'
    Wait-Until { [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_DETAILS')).Contains($zero) } `
        'Sorted rows still show the matching complete path and rule details' | Out-Null
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_ALL'
    $unfilteredTotals = Get-CleanupTotalsWithoutVisible $dialog
    Click-CleanupFilter $dialog $true
    Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) ($expected.Count - 1) 'The zero-allocation filter hides exactly one row'
    Assert-True ($zero -notin [NativeListViewHelper]::GetItemTexts($files)) 'The zero-allocation candidate is hidden'
    Assert-Equal (Get-CleanupTotalsWithoutVisible $dialog) $unfilteredTotals `
        'Filtering changes neither total allocation nor hidden checked candidates'
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($categories, 1)) $counts `
        'Filtering does not change category counts'
    for ($toggle = 0; $toggle -lt 3; ++$toggle) {
        Click-CleanupFilter $dialog $false -Label
        Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) $expected.Count 'Repeated native clicks restore empty files'
        Click-CleanupFilter $dialog $true -Label
        Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) ($expected.Count - 1) 'Repeated native clicks hide empty files'
        Assert-Equal (Get-CleanupTotalsWithoutVisible $dialog) $unfilteredTotals 'Repeated toggles preserve all original check states and totals'
    }
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_NONE'
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_ALL'
    Click-CleanupFilter $dialog $false
    Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) $expected.Count 'Disabling the filter restores every candidate'
    $zeroRow = [array]::IndexOf([NativeListViewHelper]::GetItemTexts($files), $zero)
    Assert-Equal ([TestDesktop]::Message($files, 0x102C, $zeroRow, 0xF000).ToInt32() -band 0xF000) 0x1000 `
        'Clear all includes hidden candidates and select visible does not check them again'
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_NONE'
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
    Assert-Equal $afterExclusion.Count 7 'Excluding one directory removes exactly one candidate'
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
    Assert-True ([NativeListViewHelper]::GetItemTexts($categories, 4)[0] -match 'partial|fail|部分|失败|不完整') `
        'The affected Temp category exposes a read failure rather than an empty result'
    Set-Acl -LiteralPath $blocked -AclObject $acl
    $restored = Get-Acl -LiteralPath $blocked
    Assert-Equal ($restored.GetOwner([Security.Principal.SecurityIdentifier]).Value) `
        ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value) 'The synthetic directory owner is restored'
    Assert-Equal ($restored.GetGroup([Security.Principal.SecurityIdentifier]).Value) `
        ($acl.GetGroup([Security.Principal.SecurityIdentifier]).Value) 'The synthetic directory group is restored'
    Assert-Equal $restored.AreAccessRulesProtected $acl.AreAccessRulesProtected 'Directory inheritance protection is restored'
    Assert-Sequence (Get-CleanupAccessRules $restored) (Get-CleanupAccessRules $acl) `
        'Every access rule is restored, including rights, inheritance and deny state'

    # Exercise the actual preview cap, including label clicks after a full list rebuild.
    for ($index = 0; $index -lt 10000; ++$index) {
        New-CleanupFixtureFile (Join-Path $local "Temp\stress\file-$index.tmp") 0 | Out-Null
    }
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $dialog
    Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) 10000 'The synthetic preview reaches its real candidate cap'
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_ALL'
    $cappedTotals = Get-CleanupTotalsWithoutVisible $dialog
    for ($toggle = 0; $toggle -lt 2; ++$toggle) {
        Click-CleanupFilter $dialog $true -Label
        Assert-True ([NativeListViewHelper]::GetItemCount($files) -lt 10000) 'A label click hides empty files at the candidate cap'
        Click-CleanupFilter $dialog $false -Label
        Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) 10000 'A second label click restores all capped candidates'
        Assert-Equal (Get-CleanupTotalsWithoutVisible $dialog) $cappedTotals 'Capped list toggles preserve candidate totals and original checks'
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
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 9; FontSize = 100 }
Register-Scenario cleanup.preview.zh CleanupPreview 'Chinese read-only cleanup preview and failure explanations' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 30724; FontSize = 200 }
