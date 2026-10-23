function Test-NuGetHttpFlow {
    param($App, [IntPtr] $Preview, [string] $Root, $Context, [switch] $Pip)
    $http = Join-Path $Root $(if ($Pip) { 'PipCache' } else { 'NuGetHttp' })
    $entry = if ($Pip) { 'IDC_CP_PIP' } else { 'IDC_CP_NUGET' }
    $command = if ($Pip) { 'cache purge' } else { 'http-cache --clear' }
    $imagePrefix = if ($Pip) { 'pip-cache' } else { 'nuget-http' }
    $a = New-TestFile (Join-Path $http $(if ($Pip) { 'http-v2\recent.body' } else { 'source\recent.dat' })) 12345
    $b = New-TestFile (Join-Path $http $(if ($Pip) { 'wheels\hash\synthetic-1-py3-none-any.whl' } else { 'source\unchecked.dat' })) 6789
    $sentinels = @('packages','scratch','plugins','project' | ForEach-Object {
        New-TestFile (Join-Path $Root "$_\nuget-sentinel") 4097
    })
    $hashes = @($sentinels | Get-FileHash | ForEach-Object Hash)
    $httpHashes = @($a,$b | Get-FileHash | ForEach-Object Hash)
    foreach ($rule in @($http, (Join-Path $http 'source'), 'relative-directory')) {
        Set-CleanupExclusions $Preview $rule
        [void][TestDesktop]::PostMessage((Get-DialogControl $Preview $entry), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        $blocked = Wait-Until { Get-AppDialog $App 'IDC_NH_CLEAR' } 'Official scope review opens with current exclusions'
        $clearBlocked = Get-DialogControl $blocked 'IDC_NH_CLEAR'
        $stopBlocked = Get-DialogControl $blocked 'IDC_NH_STOP'
        Wait-Until { -not [TestDesktop]::IsWindowEnabled($stopBlocked) } 'Exclusion protection settles' -StableMilliseconds 350 | Out-Null
        Assert-True (-not [TestDesktop]::IsWindowEnabled($clearBlocked)) 'Conflicting or invalid exclusion rules disable confirmation'
        $status = [TestDesktop]::ControlText((Get-DialogControl $blocked 'IDC_NH_STATUS'))
        Assert-True ($status -match 'excluded directory|exclusion rules|排除目录|排除规则') 'The reason identifies exclusion protection'
        if ($rule -ne 'relative-directory') {
            $detail = [TestDesktop]::ControlText((Get-DialogControl $blocked 'IDC_NH_DETAILS'))
            Assert-True ($detail.Contains($rule)) 'The conflicting excluded directory is shown'
        }
        # Sending the command directly must not bypass the disabled control or create a confirmation.
        [void][TestDesktop]::Message($blocked, 0x0111, (Get-ResourceId 'IDC_NH_CLEAR'), $clearBlocked.ToInt64())
        Assert-Equal @(Get-CleanupWindowsPrompt $App $Preview).Count 0 'Blocked scope has no confirmation override'
        Assert-Sequence @($a,$b | Get-FileHash | ForEach-Object Hash) $httpHashes 'Exclusion conflicts and detection failure clear no synthetic file'
        Close-AppDialog $blocked
        Wait-Until { [TestDesktop]::IsWindowEnabled($Preview) } 'Blocked scope returns without running cleanup' | Out-Null
    }
    Set-CleanupExclusions $Preview (Join-Path $Root 'project')
    foreach ($attempt in 'success','failure') {
        [void][TestDesktop]::PostMessage((Get-DialogControl $Preview $entry), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        $dialog = Wait-Until { Get-AppDialog $App 'IDC_NH_CLEAR' } 'Independent official cache scope dialog opens'
        $clear = Get-DialogControl $dialog 'IDC_NH_CLEAR'
        $stop = Get-DialogControl $dialog 'IDC_NH_STOP'
        Wait-Until { -not [TestDesktop]::IsWindowEnabled($stop) } 'Official command discovery finishes' -StableMilliseconds 350 | Out-Null
        $detailsControl = Get-DialogControl $dialog 'IDC_NH_DETAILS'
        $details = [TestDesktop]::ControlText($detailsControl)
        $statusControl = Get-DialogControl $dialog 'IDC_NH_STATUS'
        Assert-True ([TestDesktop]::IsWindowEnabled($clear)) ('Installed SDK and isolated scope are usable: ' + $details)
        Assert-True ($details.Contains($http) -and $details.Contains($command)) 'Actual official scope and fixed command are displayed'
        $notice = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_NH_NOTICE'))
        Assert-True ($notice -match 'ENTIRE|整个|全量') 'The entire HTTP cache scope is explicit'
        Assert-True ($notice -match 'unchecked|未勾选') 'Official cleanup is independent of preview selection'
        Assert-True ($notice -match 'excluded directory|排除目录') 'Exclusion overlap is an explicit non-overridable guard'
        Assert-True ($notice -match 'undone|撤销') 'Irreversible impact is visible before confirmation'
        $bounds = [TestDesktop]::Rectangle($dialog)
        $work = [System.Windows.Forms.Screen]::FromHandle($dialog).WorkingArea
        Assert-True ($bounds[0] -ge $work.Left -and $bounds[1] -ge $work.Top -and $bounds[2] -le $work.Right -and $bounds[3] -le $work.Bottom) 'The scope review fits the desktop'
        if ($attempt -eq 'success') {
            $totals = [TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_TOTALS'))
            [void][TestDesktop]::PostMessage($clear, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
            $prompt = Wait-Until { Get-CleanupWindowsPrompt $App $Preview } 'Official cleanup requires a separate confirmation'
            Assert-Equal ([TestDesktop]::Message($prompt, 0x0400).ToInt32() -band 0xFFFF) 2 'Cancel is the default action before actual cleanup'
            $text = @([TestDesktop]::Children($prompt) | ForEach-Object { [TestDesktop]::ControlText($_) }) -join "`n"
            Assert-True ($text.Contains($http) -and $text -match 'Recycle Bin|回收站') 'Confirmation names the actual cache and lack of Recycle Bin recovery'
            Close-AppDialog $prompt
            Wait-Until { [TestDesktop]::IsWindowEnabled($dialog) } 'Cancelling returns to scope review' | Out-Null
            Wait-Until { [TestDesktop]::ControlText($statusControl) -match 'Confirmation cancelled|已取消确认' } 'Pre-start cancellation is reported' | Out-Null
            Assert-True ((Test-Path -LiteralPath $a) -and (Test-Path -LiteralPath $b)) 'Confirmation cancellation clears no file'
            Assert-Equal ([TestDesktop]::ControlText((Get-DialogControl $Preview 'IDC_CP_TOTALS'))) $totals 'Cancelled official operation preserves preview totals'
            Save-Frame ([TestDesktop]::Capture($dialog)) (Join-Path $Context.Root ($imagePrefix + '-scope.png'))
        }
        $locked = $null
        try {
            if ($attempt -eq 'failure') { $locked = [IO.File]::Open($a, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read) }
            [void][TestDesktop]::PostMessage($clear, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
            $prompt = Wait-Until { Get-CleanupWindowsPrompt $App $Preview } 'Actual synthetic cleanup still requires explicit confirmation'
            Close-AppDialog $prompt 1
            Wait-Until { -not [TestDesktop]::IsWindowEnabled($stop) -and -not [TestDesktop]::IsWindowEnabled($clear) -and
                [TestDesktop]::ControlText($detailsControl) -match 'Exit code|退出码' } 'Official command returns its result and observations' -StableMilliseconds 350 | Out-Null
            $status = [TestDesktop]::ControlText($statusControl)
            $result = [TestDesktop]::ControlText($detailsControl)
            if ($attempt -eq 'success') {
                Assert-True ($status -match 'command succeeded|命令执行成功') 'Command success is presented separately from space claims'
                Assert-True (-not (Test-Path -LiteralPath $a) -and -not (Test-Path -LiteralPath $b)) 'Confirmed official operation clears both recent and unchecked synthetic files'
                Assert-True ($result -match '19134' -and $result -match 'after: 0 B|清理后缓存字节：0 B') 'Measured cache bytes show actual contents before and after'
                Save-Frame ([TestDesktop]::Capture($dialog)) (Join-Path $Context.Root ($imagePrefix + '-result.png'))
            } else {
                Assert-True ($status -match 'failed|失败') 'Locked-file official failure is visible'
                Assert-True (Test-Path -LiteralPath $a) 'A locked file survives without permission changes or fallback deletion'
            }
            Assert-True ($result -match 'not attributed released space|不能归因') 'Observed volume changes are not advertised as released space'
            Assert-Sequence @($sentinels | Get-FileHash | ForEach-Object Hash) $hashes 'Packages, scratch, plugins and project data remain unchanged'
        } finally { if ($locked) { $locked.Dispose() } }
        Close-AppDialog $dialog
        Wait-Until { [TestDesktop]::IsWindowEnabled($Preview) } 'The official result returns to the preview' | Out-Null
        Assert-Equal ([NativeListViewHelper]::GetItemCount((Get-DialogControl $Preview 'IDC_CP_FILES'))) 0 'An executed official operation invalidates stale candidate rows'
        if ($attempt -eq 'success') {
            $a = New-TestFile (Join-Path $http $(if ($Pip) { 'http-v2\locked.body' } else { 'source\locked.dat' })) 12345
            New-TestFile (Join-Path $http $(if ($Pip) { 'wheels\hash\free-1-py3-none-any.whl' } else { 'source\free.dat' })) 6789 | Out-Null
        }
    }
    Set-CleanupExclusions $Preview ''
}
