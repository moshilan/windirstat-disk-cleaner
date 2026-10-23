function New-CleanupFixtureFile {
    param([string] $Path, [int] $Size = 4097, [switch] $Recent, [int] $AgeDays = 10)
    New-TestFile $Path $Size | Out-Null
    $time = if ($Recent) { [datetime]::UtcNow } else { [datetime]::UtcNow.AddDays(-$AgeDays) }
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

function Get-CleanupWindowsPrompt {
    param($App, [IntPtr] $Preview)
    return @([TestDesktop]::Windows($App.Process.Id) | Where-Object {
        $_ -ne $Preview -and [TestDesktop]::Class($_) -eq '#32770' -and [TestDesktop]::IsWindowVisible($_) -and
        [TestDesktop]::GetDlgItem($_, 1) -ne [IntPtr]::Zero -and [TestDesktop]::GetDlgItem($_, 2) -ne [IntPtr]::Zero
    } | Select-Object -First 1)
}

function Test-CleanupWindowsCancel {
    param($App, [IntPtr] $Preview)
    $files = Get-DialogControl $Preview 'IDC_CP_FILES'
    $before = @([NativeListViewHelper]::GetItemTexts($files))
    $totals = [TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_TOTALS'))
    $notice = [TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_WINDOWS_NOTICE'))
    Assert-True ($notice -match 'separate|独立') 'Windows cleanup scope is explicitly separate from third-party candidates'
    $first = [TestDesktop]::Rectangle((Get-DialogControl $Preview 'IDC_CP_WINDOWS_RECOMMENDATIONS'))
    $second = [TestDesktop]::Rectangle((Get-DialogControl $Preview 'IDC_CP_WINDOWS_DISK'))
    Assert-True ($first[2] -lt $second[0]) 'The two Windows entry labels have separate controls'
    $bounds = [TestDesktop]::Rectangle($Preview)
    foreach ($id in 'IDC_CP_SCAN','IDC_CP_CANCEL','IDC_CP_SELECT_ALL','IDC_CP_SELECT_NONE') {
        $button = [TestDesktop]::Rectangle((Get-DialogControl $Preview $id))
        Assert-True ($button[3] -lt $bounds[3] - 3) 'Every bottom action fits inside the preview frame'
    }
    $close = [TestDesktop]::Rectangle(([TestDesktop]::GetDlgItem($Preview, 2)))
    Assert-True ($close[3] -lt $bounds[3] - 3) 'The standard Close button fits inside the preview frame'
    foreach ($control in 'IDC_CP_WINDOWS_RECOMMENDATIONS','IDC_CP_WINDOWS_DISK') {
        $processes = @('SystemSettings','cleanmgr' | ForEach-Object { [Diagnostics.Process]::GetProcessesByName($_) } | ForEach-Object Id)
        [void][TestDesktop]::PostMessage((Get-DialogControl $Preview $control), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        $prompt = Wait-Until { Get-CleanupWindowsPrompt $App $Preview } 'Windows tool launch requires an explicit user confirmation'
        Assert-Equal ([TestDesktop]::Message($prompt, 0x0400).ToInt32() -band 0xFFFF) 2 'Cancel is the default action before launching Windows'
        Close-AppDialog $prompt
        Wait-Until { [TestDesktop]::IsWindowEnabled($Preview) } 'Cancelling returns to the preview' | Out-Null
        Assert-True ([TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_STATUS')) -match 'cancelled|已取消') 'Cancel is reported without claiming cleanup completion'
        $after = @('SystemSettings','cleanmgr' | ForEach-Object { [Diagnostics.Process]::GetProcessesByName($_) } | ForEach-Object Id)
        Assert-Equal @($after | Where-Object { $_ -notin $processes }).Count 0 'Cancel starts neither Windows cleanup provider'
    }
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($files)) $before 'Windows launch cancellation preserves candidate identities'
    Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_TOTALS'))) $totals 'Windows launch cancellation preserves selection and byte estimates'
}

function Test-CleanupWindowsLaunch {
    param($App, [IntPtr] $Preview)
    # Only the disposable cloud desktop opens real Windows tools. Never confirm cleanup there.
    if ($env:GITHUB_ACTIONS -ne 'true') { Write-TestNote 'Actual Windows provider launch is cloud-only'; return }
    $files = Get-DialogControl $Preview 'IDC_CP_FILES'
    $before = @([NativeListViewHelper]::GetItemTexts($files))
    $totals = [TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_TOTALS'))
    foreach ($entry in @(
        @{ Control = 'IDC_CP_WINDOWS_RECOMMENDATIONS'; Process = 'SystemSettings' },
        @{ Control = 'IDC_CP_WINDOWS_DISK'; Process = 'cleanmgr' }
    )) {
        $visible = @(@($entry.Process, 'ApplicationFrameHost') | ForEach-Object { [Diagnostics.Process]::GetProcessesByName($_) } | ForEach-Object {
            [TestDesktop]::Windows($_.Id) | Where-Object { [TestDesktop]::IsWindowVisible($_) }
        })
        [void][TestDesktop]::PostMessage((Get-DialogControl $Preview $entry.Control), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        $prompt = Wait-Until { Get-CleanupWindowsPrompt $App $Preview } 'The explicit Windows launch confirmation appears'
        Close-AppDialog $prompt 1
        Wait-Until { [TestDesktop]::IsWindowEnabled($Preview) } 'The launch request returns to the preview' | Out-Null
        $status = Wait-Until {
            $text = [TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_STATUS'))
            if ($text -match 'Launch requested|已发送启动请求|Could not open|无法打开|unavailable|不可用') { $text }
        } 'Wait for the asynchronous provider activation request result' -Seconds 30 -StableMilliseconds 350
        Assert-True ($status -match 'Launch requested|已发送启动请求') "A successful launch is reported only as a request (observed: $status)"
        $processName = $entry.Process
        $window = Wait-Until {
            $providerIds = @([Diagnostics.Process]::GetProcessesByName($processName) | ForEach-Object Id)
            $roots = @($providerIds | ForEach-Object { [TestDesktop]::Windows($_) | ForEach-Object { [TestDesktop]::RootWindow($_) } })
            if ($processName -eq 'SystemSettings') {
                $roots += @([Diagnostics.Process]::GetProcessesByName('ApplicationFrameHost') | ForEach-Object {
                    [TestDesktop]::Windows($_.Id) | Where-Object {
                        @([TestDesktop]::Children($_) | Where-Object { [TestDesktop]::ProcessId($_) -in $providerIds }).Count -gt 0
                    }
                })
            }
            @($roots | Select-Object -Unique | Where-Object { $_ -notin $visible -and [TestDesktop]::IsWindowVisible($_) } | Select-Object -First 1)
        } "The official $processName provider creates a visible window" -Seconds 30 -StableMilliseconds 750
        $ownerProcess = [TestDesktop]::ProcessId($window)
        Write-TestNote "Official $processName window: class=$([TestDesktop]::Class($window)); title=$([TestDesktop]::Text($window)); close=SC_CLOSE; page location not asserted"
        Add-Cleanup 'Close only the Windows provider window opened by this scenario' {
            param($Window, $OwnerProcess)
            if ([TestDesktop]::IsWindow($Window) -and [TestDesktop]::ProcessId($Window) -eq $OwnerProcess) {
                [void][TestDesktop]::PostMessage($Window, 0x0112, [IntPtr]0xF060, [IntPtr]::Zero)
            }
        } @($window, $ownerProcess)
        # Close the real frame via its system menu, including hosted Settings. Never click cleanup/OK.
        [void][TestDesktop]::PostMessage($window, 0x0112, [IntPtr]0xF060, [IntPtr]::Zero)
        Wait-Until { -not [TestDesktop]::IsWindow($window) -or -not [TestDesktop]::IsWindowVisible($window) } 'Closing the provider cancels without confirming cleanup' -Seconds 30 | Out-Null
        Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_STATUS'))) $status 'Provider cancellation never reports cleanup completion or released space'
    }
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($files)) $before 'Official providers do not replace third-party preview candidates'
    Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_TOTALS'))) $totals 'Official providers do not change preview estimates'
}

function Test-CleanupReadOnlyPlan {
    param($App, [IntPtr] $Preview, [string[]] $Expected)
    $open = Get-DialogControl $Preview 'IDC_CP_PLAN'
    Assert-True ([TestDesktop]::IsWindowEnabled($open)) 'A complete selected preview can create a plan'
    [void][TestDesktop]::PostMessage($open, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    $plan = Wait-Until { Get-AppDialog $App 'IDC_CPLAN_SIMULATE' } 'The independent read-only plan opens'
    $files = Get-DialogControl $plan 'IDC_CPLAN_FILES'
    $stop = Get-DialogControl $plan 'IDC_CPLAN_STOP'
    $wait = {
        Wait-Until {
            -not [TestDesktop]::IsWindowEnabled($stop) -and [NativeListViewHelper]::GetItemCount($files) -eq $Expected.Count
        } 'The plan worker returns its complete immutable snapshot' -StableMilliseconds 350 | Out-Null
    }
    & $wait
    $rect = [TestDesktop]::Rectangle($plan)
    $work = [System.Windows.Forms.Screen]::FromHandle($plan).WorkingArea
    Assert-True ($rect[0] -ge $work.Left -and $rect[1] -ge $work.Top -and $rect[2] -le $work.Right -and $rect[3] -le $work.Bottom) `
        'The read-only plan fits its monitor working area'
    $lastButton = [TestDesktop]::Rectangle((Get-DialogControl $plan 'IDC_CPLAN_STOP'))
    $closeButton = [TestDesktop]::Rectangle(([TestDesktop]::GetDlgItem($plan, 2)))
    Assert-True ($lastButton[2] -lt $closeButton[0]) 'Plan verification and close controls do not overlap'
    Assert-True (-not [TestDesktop]::IsWindowEnabled($Preview)) 'The source selection cannot change while reviewing its plan'
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($files) | Sort-Object) @($Expected | Sort-Object) `
        'The plan contains exactly the explicitly selected preview files'
    foreach ($column in 1,2,3,4,5,6,7) {
        Assert-Equal @([NativeListViewHelper]::GetItemTexts($files, $column) | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count 0 `
            "Plan column $column explains rule, evidence, risk, disposition, estimate, result or error"
    }
    $mode = Get-DialogControl $plan 'IDC_CPLAN_MODE'
    Assert-Equal ([TestDesktop]::Message($mode, 0x0147).ToInt32()) 0 'A new plan defaults explicitly to the Recycle Bin'
    Assert-Equal @([NativeListViewHelper]::GetItemTexts($files, 5) | Where-Object { $_ -notmatch '^0' }).Count 0 `
        'Recycle intent does not claim released allocation'
    $buttons = @([TestDesktop]::Children($plan) | Where-Object { [TestDesktop]::Class($_) -eq 'Button' } |
        ForEach-Object { [TestDesktop]::ControlText($_) })
    Assert-True (-not @($buttons | Where-Object { $_ -match '删除|执行清理|Delete|Clean now' }).Count) `
        'The plan exposes simulation, stop and close without a deletion button'
    Invoke-CleanupButton $plan 'IDC_CPLAN_SIMULATE'
    & $wait
    Assert-Equal @([NativeListViewHelper]::GetItemTexts($files, 6) | Where-Object { $_ -notmatch 'Simulation passed|模拟复核通过' }).Count 0 `
        'All unchanged selected files pass read-only metadata simulation'
    $recycleSummary = [TestDesktop]::ControlText((Get-DialogControl $plan 'IDC_CPLAN_STATUS'))
    Assert-True ($recycleSummary -match 'Actual released: 0|实际释放0') 'Simulation always reports zero actual release'
    foreach ($index in 1,2) {
        [void][TestDesktop]::Message($mode, 0x014E, $index)
        [void][TestDesktop]::Message($plan, 0x0111, ((1 -shl 16) -bor (Get-ResourceId 'IDC_CPLAN_MODE')), $mode.ToInt64())
        & $wait
        Assert-Equal ([TestDesktop]::Message($mode, 0x0147).ToInt32()) $index 'Only an explicit mode selection changes disposition'
        if ($index -eq 2) {
            $summary = [TestDesktop]::ControlText((Get-DialogControl $plan 'IDC_CPLAN_STATUS'))
            Assert-True ($summary -match "Skipped/failed: $($Expected.Count)\b|跳过或失败$($Expected.Count)\D") `
                'An unsupported plan counts all skipped items before simulation'
        }
        Invoke-CleanupButton $plan 'IDC_CPLAN_SIMULATE'
        & $wait
        $results = @([NativeListViewHelper]::GetItemTexts($files, 6))
        if ($index -eq 1) {
            Assert-Equal @($results | Where-Object { $_ -notmatch 'Simulation passed|模拟复核通过' }).Count 0 `
                'Explicit permanent intent remains a read-only simulation'
            Assert-True (@([NativeListViewHelper]::GetItemTexts($files, 5) | Where-Object { $_ -notmatch '^0' }).Count -gt 0) `
                'Permanent intent reports a nonzero allocation upper bound for nonempty fixtures'
        } else {
            Assert-Equal @($results | Where-Object { $_ -notmatch 'Unsupported disposition|不支持此处置方式' }).Count 0 `
                'Official Windows cleanup is skipped with its unsupported reason'
            Assert-Equal @([NativeListViewHelper]::GetItemTexts($files, 4) | Where-Object { $_ -notmatch 'Windows' }).Count 0 `
                'Unsupported official cleanup never switches to file deletion'
        }
        Assert-True ([TestDesktop]::ControlText((Get-DialogControl $plan 'IDC_CPLAN_STATUS')) -match 'Actual released: 0|实际释放0') `
            'Every disposition keeps actual released bytes at zero'
    }
    Close-AppDialog $plan
    Assert-True ([TestDesktop]::IsWindowEnabled($Preview)) 'Closing the plan restores its original preview'
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

function Start-CleanupSettingsPreview {
    param($Runner)
    $app = Start-TestApp -Runner $Runner
    $selection = Wait-Until { Get-AppDialog $app 'IDC_SCAN_DUPLICATES' } 'A settings restart opens target selection without scanning'
    Close-AppDialog $selection
    Wait-AppIdle $app
    Invoke-AppCommand $app 'ID_CLEANUP_PREVIEW' -Unchecked
    $dialog = Wait-Until { Get-AppDialog $app 'IDC_CP_SCAN' } 'The settings preview opens after a real process restart'
    return [pscustomobject]@{ App = $app; Dialog = $dialog }
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
    param([IntPtr] $Dialog, [bool] $Expected, [switch] $Label, [switch] $Measure)
    $button = Get-DialogControl $Dialog 'IDC_CP_HIDE_ZERO'
    $rect = [TestDesktop]::Rectangle($button)
    [TestDesktop]::Activate($Dialog)
    $x = $rect[0] + $(if ($Label) { 60 } else { 8 })
    $y = [int](($rect[1] + $rect[3]) / 2)
    [TestDesktop]::MoveCursor($x, $y)
    Assert-Equal ([TestDesktop]::WindowAt($x, $y)) $button 'The native filter pointer reaches the checkbox or its label'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    [TestDesktop]::LeftButton($true)
    Start-Sleep -Milliseconds 80
    [TestDesktop]::LeftButton($false)
    Wait-Until { [TestDesktop]::Message($button, 0x00F0).ToInt32() -eq [int]$Expected } `
        'A native filter click reaches the requested checked state' -StableMilliseconds 150 | Out-Null
    if ($Measure) {
        $elapsed = $watch.ElapsedMilliseconds
        $timings = [ordered]@{ Hidden = $Expected; InteractionMs = $elapsed }
        foreach ($phase in @('DataUs', 'FilterUs', 'SortUs', 'ControlsUs', 'SummaryUs', 'TotalUs', 'PaintUs')) {
            $timings[$phase] = [TestDesktop]::GetWindowProperty($Dialog, ('WDS.Cleanup.Refresh.' + $phase)).ToInt64()
        }
        Write-TestNote ('Cleanup refresh: ' + ($timings | ConvertTo-Json -Compress))
        Assert-True ($elapsed -lt 3000) 'A capped native filter interaction settles within the unchanged three-second limit'
    }
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
    $roaming = Join-Path $root 'RoamingAppData'
    [IO.Directory]::CreateDirectory($local) | Out-Null
    [IO.Directory]::CreateDirectory($roaming) | Out-Null
    [IO.File]::WriteAllText((Join-Path $root '.windirstat-cleanup-fixture'), 'WinDirStat read-only cleanup fixture v1')
    $relative = @(
        'Temp\old.tmp', 'Temp\excluded\item.tmp', 'Temp\excluded-sibling\item.temp',
        'Google\Chrome\User Data\Default\Cache\Cache_Data\entry.bin',
        'Microsoft\Edge\User Data\Profile 1\Cache\Cache_Data\entry.bin',
        'Mozilla\Firefox\Profiles\sample.default\cache2\entries\0123456789ABCDEF0123456789ABCDEF01234567',
        'pip\Cache\http-v2\a\b\c\d\e\response.body',
        'BraveSoftware\Brave-Browser\User Data\Default\Cache\Cache_Data\entry.bin',
        'Vivaldi\User Data\Profile 2\Cache\Cache_Data\entry.bin',
        'npm-cache\_cacache\content-v2\sha512\aa\payload',
        'NuGet\v3-cache\source\list.dat',
        'pip\Cache\http\a\b\c\d\e\legacy.body'
    )
    $expected = @($relative | ForEach-Object { New-CleanupFixtureFile (Join-Path $local $_) })
    $largest = New-CleanupFixtureFile (Join-Path $local 'Temp\old.tmp') 32769
    $zero = New-CleanupFixtureFile (Join-Path $local 'Temp\zero.tmp') 0
    $expected += $zero
    $codeCache = New-CleanupFixtureFile (Join-Path $roaming 'Code\CachedData\0123456789abcdef0123456789abcdef01234567\compiled.bin') -AgeDays 100
    $expected += $codeCache
    New-CleanupFixtureFile (Join-Path $local 'Temp\recent.tmp') -Recent | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Temp\personal.docx') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Google\Chrome\User Data\Default\Cookies') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Google\Chrome\User Data\Default\Service Worker\CacheStorage\personal') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'pip\Cache\wheels\package.whl') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'BraveSoftware\Brave-Browser\User Data\Default\Local Storage\wallet') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'Vivaldi\User Data\Profile 2\Cookies') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'npm-cache\_npx\project\node_modules\package\index.js') | Out-Null
    New-CleanupFixtureFile (Join-Path $local 'NuGet\plugins-cache\sentinel') | Out-Null
    New-CleanupFixtureFile (Join-Path $roaming 'Code\User\settings.json') | Out-Null
    New-CleanupFixtureFile (Join-Path $roaming 'Code\Backups\unsaved.txt') | Out-Null
    New-CleanupFixtureFile (Join-Path $roaming 'Code\CachedData\not-a-commit\personal.txt') -AgeDays 100 | Out-Null
    New-CleanupFixtureFile (Join-Path $roaming 'Code\CachedData\0123456789abcdef0123456789abcdef01234567\ten-days.bin') | Out-Null
    $before = @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | Get-FileHash)
    $desktop = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $fontSize = if ($Case.FontSize -eq 200 -and ($desktop.Width -lt 2000 -or $desktop.Height -lt 1600)) { 100 } else { $Case.FontSize }
    Write-TestNote "Native resize desktop: $($desktop.Width)x$($desktop.Height); font=$fontSize%; full-window-drag=$([System.Windows.Forms.SystemInformation]::DragFullWindows)"
    $runner = New-TestRunner @{ Options = @{ LanguageId = $Case.Language; FontSizePercent = $fontSize; DarkMode = 1 } }
    $runner | Add-Member -NotePropertyName Environment -NotePropertyValue @{ WDS_CLEANUP_TEST_ROOT = $root }
    if ($Case.ContainsKey('NuGetTools')) {
        $runner.Environment.NUGET_HTTP_CACHE_PATH = Join-Path $root 'NuGetHttp'
        $runner.Environment.NUGET_PACKAGES = Join-Path $root 'packages'
        $runner.Environment.NUGET_SCRATCH = Join-Path $root 'scratch'
        $runner.Environment.NUGET_PLUGINS_CACHE_PATH = Join-Path $root 'plugins'
    }
    if ($Case.ContainsKey('PipTools')) {
        $runner.Environment.WDS_PIP_TEST_PYTHON = (Get-Command python -CommandType Application -ErrorAction Stop).Source
        $runner.Environment.PIP_CACHE_DIR = Join-Path $root 'PipCache'
        $runner.Environment.PIP_CONFIG_FILE = 'NUL'
    }
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
    Assert-Equal ([NativeListViewHelper]::GetItemCount($categories)) 10 'Ten audited categories are available'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_CP_CANCEL'))) 'Idle preview has no running worker'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_CP_PLAN'))) 'An empty preview cannot form a plan'
    $buttons = @([TestDesktop]::Children($dialog) | Where-Object { [TestDesktop]::Class($_) -eq 'Button' } |
        ForEach-Object { [TestDesktop]::ControlText($_) })
    Assert-True (-not @($buttons | Where-Object { $_ -match '删除|执行清理|Delete|Execute|Clean now' }).Count) `
        'The preview has no self-managed deletion or plan execution button'

    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $dialog
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($files) | Sort-Object) @($expected | Sort-Object) `
        'Only audited old temporary and cache files appear in the GUI'
    $counts = @([NativeListViewHelper]::GetItemTexts($categories, 1))
    Assert-Sequence $counts @('4','1','1','1','2','1','1','1','1','1') 'Category counts include both pip layouts and zero-allocation candidates exactly once'
    $coverage = @([NativeListViewHelper]::GetItemTexts($categories, 4))
    Assert-Equal @($coverage | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count 0 `
        'Every category explains its discovery outcome'
    Assert-True ([NativeListViewHelper]::FocusListView($categories) -and [NativeListViewHelper]::SelectSingleItem($categories, 4)) `
        'The pip category can be selected for all of its audited layouts'
    $details = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_DETAILS'))
    Assert-True ($details.Contains('pip-http-v2') -and $details.Contains('pip-http-legacy-v1')) `
        'Category details explain modern and legacy pip rules together'
    $legacy = Join-Path $local 'pip\Cache\http\a\b\c\d\e\legacy.body'
    $legacyRow = [array]::IndexOf([NativeListViewHelper]::GetItemTexts($files), $legacy)
    Assert-True ([NativeListViewHelper]::FocusListView($files) -and [NativeListViewHelper]::SelectSingleItem($files, $legacyRow)) `
        'A legacy pip candidate can be selected for its actual matching rule'
    $details = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_DETAILS'))
    Assert-True ($details.Contains($legacy) -and $details.Contains('pip-http-legacy-v1')) `
        'Candidate details use the actual legacy rule instead of the category primary rule'
    $riskValues = @([NativeListViewHelper]::GetItemTexts($files, 5))
    Assert-True (@($riskValues | Where-Object { $_ -match 'regener|再生' }).Count -gt 0 -and
        @($riskValues | Where-Object { $_ -match 'review|人工' }).Count -gt 0) `
        'The list distinguishes regenerable caches from candidates requiring judgment'
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
    Test-CleanupWindowsCancel $app $dialog
    Test-CleanupReadOnlyPlan $app $dialog $expected
    Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_TOTALS'))) $allTotals `
        'Building and simulating independent plans preserves the original selection and totals'
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_NONE'
    $noneTotals = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_TOTALS'))
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_CP_PLAN'))) 'Clearing all selection disables plan creation'
    Assert-True ($allTotals -ne $noneTotals) 'Selected counts and allocation estimates update with selection'

    # Exercise the native state icon as well as the keyboard and bulk commands.
    Assert-True ([NativeListViewHelper]::FocusListView($files) -and [NativeListViewHelper]::SelectSingleItem($files, 0)) `
        'The first candidate is brought into view before its native checkbox is clicked'
    Assert-Equal ([TestDesktop]::Message($files, 0x1027).ToInt32()) 0 'The native checkbox target really is row zero'
    $fileRect = [TestDesktop]::Rectangle($files)
    $headerRect = [TestDesktop]::Rectangle([TestDesktop]::Message($files, 0x101F))
    [TestDesktop]::Activate($dialog)
    [TestDesktop]::MoveCursor($fileRect[0] + 10, $headerRect[3] + 8)
    Assert-Equal ([TestDesktop]::WindowAt($fileRect[0] + 10, $headerRect[3] + 8)) $files 'The native candidate checkbox pointer reaches the list'
    foreach ($expectedState in @(0x2000, 0x1000)) {
        [TestDesktop]::LeftButton($true)
        Start-Sleep -Milliseconds 80
        [TestDesktop]::LeftButton($false)
        Wait-Until { ([TestDesktop]::Message($files, 0x102C, 0, 0xF000).ToInt32() -band 0xF000) -eq $expectedState } `
            'A native candidate checkbox click toggles its identity state' -StableMilliseconds 150 | Out-Null
    }
    Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_TOTALS'))) $noneTotals `
        'Two native checkbox clicks restore selected counts and space'

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

    if ($Case.ContainsKey('WindowsTools') -or $Case.ContainsKey('NuGetTools') -or $Case.ContainsKey('PipTools')) {
        if ($Case.ContainsKey('WindowsTools') -and $Case.Language -eq 9) { Test-CleanupWindowsLaunch $app $dialog }
        Assert-Sequence @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | Get-FileHash | ForEach-Object Hash) `
            @($before.Hash) 'Windows launch and cancellation leave every synthetic payload unchanged'
        if ($Case.ContainsKey('NuGetTools') -or $Case.ContainsKey('PipTools')) {
            Test-NuGetHttpFlow $app $dialog $root $Context -Pip:($Case.ContainsKey('PipTools'))
            Assert-Sequence @($before.Path | Get-FileHash | ForEach-Object Hash) @($before.Hash) `
                'Official HTTP cleanup leaves all original preview and user-data sentinels unchanged'
        }
        Close-AppDialog $dialog
        Stop-TestApp $app
        return # Reuse preview/plan/layout coverage above; do not run unrelated later ACL/persistence scenarios.
    }

    $excluded = Join-Path $local 'Temp\excluded'
    Set-CleanupExclusions $dialog $excluded
    Wait-Until { [NativeListViewHelper]::GetItemCount($files) -eq 0 } 'Changing exclusions invalidates the old preview and selection' | Out-Null
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $dialog
    $afterExclusion = @([NativeListViewHelper]::GetItemTexts($files))
    Assert-Equal $afterExclusion.Count ($expected.Count - 1) 'Excluding one directory removes exactly one candidate'
    Assert-True ((Join-Path $local 'Temp\excluded-sibling\item.temp') -in $afterExclusion) `
        'A similarly named sibling is not accidentally excluded'
    Assert-True ((Join-Path $local 'Temp\excluded\item.tmp') -notin $afterExclusion) 'The excluded file is absent'
    Assert-Sequence @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | Get-FileHash | ForEach-Object Hash) `
        @($before.Hash) 'Repeated previews and selections leave every source payload unchanged'
    Set-CleanupExclusions $dialog 'relative-directory'
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_CP_STATUS')) -match 'absolute|绝对') `
        'An invalid exclusion is explained without starting a scan'
    Set-CleanupExclusions $dialog ($local + "`r`n" + $roaming)
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

    # Exercise the actual preview cap with native clicks and the shipping timeout.
    for ($index = 0; $index -lt 10000; ++$index) {
        New-CleanupFixtureFile (Join-Path $local "Temp\stress\file-$index.tmp") 0 | Out-Null
    }
    Invoke-CleanupButton $dialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $dialog
    Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) 10000 'The synthetic preview reaches its real candidate cap'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_CP_PLAN'))) 'A capped incomplete preview cannot create a plan'
    Invoke-CleanupButton $dialog 'IDC_CP_SELECT_ALL'
    $cappedTotals = Get-CleanupTotalsWithoutVisible $dialog
    for ($sort = 0; $sort -lt 2; ++$sort) {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        Invoke-CleanupButton $dialog 'IDC_CP_SORT_SIZE'
        $elapsed = $watch.ElapsedMilliseconds
        Write-TestNote "Capped cleanup sort: ${elapsed}ms"
        Assert-True ($elapsed -lt 3000) 'Capped sorting completes within the unchanged three-second limit'
        Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) 10000 'Capped sorting retains every candidate'
        Assert-Equal (Get-CleanupTotalsWithoutVisible $dialog) $cappedTotals 'Capped sorting retains checked identities and statistics'
    }
    for ($toggle = 0; $toggle -lt 2; ++$toggle) {
        Click-CleanupFilter $dialog $true -Label -Measure
        Assert-True ([NativeListViewHelper]::GetItemCount($files) -lt 10000) 'A label click hides empty files at the candidate cap'
        Click-CleanupFilter $dialog $false -Label -Measure
        Assert-Equal ([NativeListViewHelper]::GetItemCount($files)) 10000 'A second label click restores all capped candidates'
        Assert-Equal (Get-CleanupTotalsWithoutVisible $dialog) $cappedTotals 'Capped list toggles preserve candidate totals and original checks'
        Assert-Equal ([TestDesktop]::Message($files, 0x102C, 9999, 0xF000).ToInt32() -band 0xF000) 0x2000 `
            'The final restored candidate keeps its original check state without rebuilding rows'
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

    # Restart the shipping process against the same portable settings, including
    # an ANSI/ASCII INI so Unicode round trips cannot depend on a UTF-16 test INI.
    $persistentRoot = Join-Path $root 'persistent-fixture'
    $persistentLocal = Join-Path $persistentRoot 'LocalAppData'
    $persistentRoaming = Join-Path $persistentRoot 'RoamingAppData'
    $unicodeExcluded = Join-Path $persistentLocal 'Temp\排除 空格;é'
    $roamingExcluded = Join-Path $persistentRoaming 'Code\CachedData\0123456789abcdef0123456789abcdef01234567'
    $persistentFiles = @(
        (New-CleanupFixtureFile (Join-Path $unicodeExcluded 'item.tmp')),
        (New-CleanupFixtureFile (Join-Path $persistentLocal 'Temp\排除 空格;é-sibling\item.tmp')),
        (New-CleanupFixtureFile (Join-Path $roamingExcluded 'compiled.bin') -AgeDays 100)
    )
    [IO.File]::WriteAllText((Join-Path $persistentRoot '.windirstat-cleanup-fixture'), 'WinDirStat read-only cleanup fixture v1')
    $persistentHashes = @($persistentFiles | Get-FileHash | ForEach-Object Hash)
    $settingsRunner = New-TestRunner @{
        Options = @{ LanguageId = $Case.Language }
        DriveSelect = @{ FilteringExcludeDirs = 'original-analysis-only' }
    }
    $initialIni = [IO.File]::ReadAllText($settingsRunner.Ini)
    [IO.File]::WriteAllText($settingsRunner.Ini, $initialIni, [Text.Encoding]::ASCII)
    $settingsRunner | Add-Member -NotePropertyName Environment -NotePropertyValue @{ WDS_CLEANUP_TEST_ROOT = $persistentRoot }
    $session = Start-CleanupSettingsPreview $settingsRunner
    $settingsDialog = $session.Dialog
    Set-CleanupExclusions $settingsDialog ($unicodeExcluded + "\`r`n" + $roamingExcluded + "\`r`n" + $unicodeExcluded.ToUpperInvariant())
    Close-AppDialog $settingsDialog
    Wait-AppIdle $session.App
    Stop-TestApp $session.App

    $session = Start-CleanupSettingsPreview $settingsRunner
    $settingsDialog = $session.Dialog
    $settingsFiles = Get-DialogControl $settingsDialog 'IDC_CP_FILES'
    $savedText = $unicodeExcluded + "`r`n" + $roamingExcluded
    Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $settingsDialog 'IDC_CP_EXCLUSIONS'))) $savedText `
        'A real restart restores normalized, deduplicated Unicode exclusions from an ASCII portable INI'
    Assert-Equal ([NativeListViewHelper]::GetItemCount($settingsFiles)) 0 'Restoring saved exclusions never starts a scan'
    Invoke-CleanupButton $settingsDialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $settingsDialog
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($settingsFiles)) @($persistentFiles[1]) `
        'Restarted exclusions suppress both anchors while preserving the same-prefix sibling'
    Set-CleanupExclusions $settingsDialog 'relative-invalid-draft'
    Assert-Equal ([NativeListViewHelper]::GetItemCount($settingsFiles)) 0 'An invalid exclusion draft invalidates old results immediately'
    Close-AppDialog $settingsDialog
    Wait-AppIdle $session.App
    Stop-TestApp $session.App

    $session = Start-CleanupSettingsPreview $settingsRunner
    $settingsDialog = $session.Dialog
    Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $settingsDialog 'IDC_CP_EXCLUSIONS'))) $savedText `
        'An invalid draft does not replace the last valid saved exclusion list'
    Set-CleanupExclusions $settingsDialog ''
    Close-AppDialog $settingsDialog
    Wait-AppIdle $session.App
    Stop-TestApp $session.App

    $session = Start-CleanupSettingsPreview $settingsRunner
    $settingsDialog = $session.Dialog
    $settingsFiles = Get-DialogControl $settingsDialog 'IDC_CP_FILES'
    Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $settingsDialog 'IDC_CP_EXCLUSIONS'))) '' `
        'Clearing exclusions survives another process restart'
    Invoke-CleanupButton $settingsDialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $settingsDialog
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($settingsFiles) | Sort-Object) @($persistentFiles | Sort-Object) `
        'A saved empty list restores all eligible synthetic candidates'
    Assert-True ([IO.File]::ReadAllText($settingsRunner.Ini).Contains('FilteringExcludeDirs=original-analysis-only')) `
        'Cleanup persistence leaves the original space-analysis exclusion setting unchanged'
    $iniAttributes = [IO.File]::GetAttributes($settingsRunner.Ini)
    $iniBeforeFailure = [IO.File]::ReadAllText($settingsRunner.Ini)
    try {
        [IO.File]::SetAttributes($settingsRunner.Ini, $iniAttributes -bor [IO.FileAttributes]::ReadOnly)
        Set-CleanupExclusions $settingsDialog $unicodeExcluded
        $saveFailure = [TestDesktop]::ControlText((Get-DialogControl $settingsDialog 'IDC_CP_STATUS'))
        Assert-True ($saveFailure -match 'sav|保存') 'An unwritable synthetic INI produces a visible persistence failure'
        if ([TestDesktop]::IsWindowEnabled((Get-DialogControl $settingsDialog 'IDC_CP_SCAN'))) {
            Invoke-CleanupButton $settingsDialog 'IDC_CP_SCAN'
        }
        Assert-Equal ([NativeListViewHelper]::GetItemCount($settingsFiles)) 0 'A failed exclusion save cannot silently reuse stale preview results'
        Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $settingsDialog 'IDC_CP_CANCEL'))) `
            'A failed exclusion save does not launch a worker'
        Assert-Equal ([IO.File]::ReadAllText($settingsRunner.Ini)) $iniBeforeFailure `
            'A failed save preserves the existing portable configuration'
    } finally { [IO.File]::SetAttributes($settingsRunner.Ini, $iniAttributes) }
    Set-CleanupExclusions $settingsDialog ''
    Close-AppDialog $settingsDialog
    Wait-AppIdle $session.App
    Stop-TestApp $session.App

    $corruptRunner = New-TestRunner @{
        Options = @{ LanguageId = $Case.Language }
        CleanupPreview = @{ ExcludedDirectoriesUtf16Hex = 'v1:00G0' }
    }
    $corruptRunner | Add-Member -NotePropertyName Environment -NotePropertyValue @{ WDS_CLEANUP_TEST_ROOT = $persistentRoot }
    $session = Start-CleanupSettingsPreview $corruptRunner
    $settingsDialog = $session.Dialog
    $settingsFiles = Get-DialogControl $settingsDialog 'IDC_CP_FILES'
    if ([TestDesktop]::IsWindowEnabled((Get-DialogControl $settingsDialog 'IDC_CP_SCAN'))) {
        Invoke-CleanupButton $settingsDialog 'IDC_CP_SCAN'
    }
    $corruptStatus = [TestDesktop]::ControlText((Get-DialogControl $settingsDialog 'IDC_CP_STATUS'))
    Assert-True ($corruptStatus -match 'invalid|corrupt|无效|损坏') 'Corrupt saved exclusions are explained instead of silently dropped'
    Assert-Equal ([NativeListViewHelper]::GetItemCount($settingsFiles)) 0 'Corrupt saved exclusions never permit a scan with an empty fallback'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $settingsDialog 'IDC_CP_CANCEL'))) `
        'Corrupt saved exclusions start no worker'
    Set-CleanupExclusions $settingsDialog ''
    Invoke-CleanupButton $settingsDialog 'IDC_CP_SCAN'
    Wait-CleanupPreviewIdle $settingsDialog
    Assert-Equal ([NativeListViewHelper]::GetItemCount($settingsFiles)) $persistentFiles.Count `
        'Explicitly clearing a corrupt exclusion setting permits an ordinary read-only preview'
    Close-AppDialog $settingsDialog
    Wait-AppIdle $session.App
    Stop-TestApp $session.App
    Assert-Sequence @($persistentFiles | Get-FileHash | ForEach-Object Hash) $persistentHashes `
        'Persistence, restart, invalid input and save-failure checks leave every synthetic source file unchanged'

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
Register-Scenario cleanup.windows CleanupPreview 'Windows entry launch/cancel and existing synthetic preview regression' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 9; FontSize = 100; WindowsTools = $true }
Register-Scenario cleanup.windows.zh CleanupPreview 'Chinese Windows entry cancellation and preview regression' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 30724; FontSize = 200; WindowsTools = $true }
Register-Scenario cleanup.nuget CleanupPreview 'Official NuGet HTTP scope, cancel, cleanup and failure on synthetic data' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 9; FontSize = 100; NuGetTools = $true }
Register-Scenario cleanup.nuget.zh CleanupPreview 'Chinese official NuGet HTTP confirmation and synthetic cleanup' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 30724; FontSize = 200; NuGetTools = $true }
Register-Scenario cleanup.pip CleanupPreview 'Selected Python official pip purge, exclusions, cancel and failure on synthetic cache' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 9; FontSize = 100; PipTools = $true }
Register-Scenario cleanup.pip.zh CleanupPreview 'Chinese selected Python pip cache scope and confirmation' `
    Test-CleanupPreview -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 30724; FontSize = 200; PipTools = $true }
