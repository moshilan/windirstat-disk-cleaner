# Synthetic application-registration fixtures only. No real inventory, uninstall, folder scan or external shell launch.
function New-InstalledAppRecord {
    param([string] $Root, [string] $Source, [string] $Id, [string] $Name, [string] $Location = '',
        [string] $Publisher = 'Fixture Vendor', [string] $Version = '1.0', [int] $SizeKiB = 0, [switch] $BadSize)
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($Root + '\' + $Source + '\' + $Id)
    try {
        foreach ($field in @{ DisplayName = $Name; Publisher = $Publisher; DisplayVersion = $Version; InstallLocation = $Location;
            UninstallString = 'NEVER_EXECUTE_INSTALLED_APP_SENTINEL /quiet'; QuietUninstallString = 'NEVER_EXECUTE_INSTALLED_APP_SENTINEL /silent' }.GetEnumerator()) {
            $key.SetValue($field.Key, $field.Value, [Microsoft.Win32.RegistryValueKind]::String)
        }
        if ($BadSize) { $key.SetValue('EstimatedSize', 'not-a-size', [Microsoft.Win32.RegistryValueKind]::String) }
        else { $key.SetValue('EstimatedSize', $SizeKiB, [Microsoft.Win32.RegistryValueKind]::DWord) }
    } finally { $key.Dispose() }
}
function Wait-InstalledAppsIdle {
    param([IntPtr] $Dialog)
    Wait-Until {
        [TestDesktop]::IsWindowEnabled((Get-DialogControl $Dialog 'IDC_IA_REFRESH')) -and
        -not [TestDesktop]::IsWindowEnabled((Get-DialogControl $Dialog 'IDC_IA_STOP'))
    } 'Application metadata worker has joined and published its result' -StableMilliseconds 200 | Out-Null
}
function Set-InstalledAppsSearch {
    param([IntPtr] $Dialog, [string] $Value)
    $edit = Get-DialogControl $Dialog 'IDC_IA_SEARCH'
    [TestDesktop]::SetText($edit, $Value)
    [void][TestDesktop]::Message($Dialog, 0x0111, ((0x0300 -shl 16) -bor (Get-ResourceId 'IDC_IA_SEARCH')), $edit.ToInt64())
}
function Start-InstalledAppsFixture {
    param($Runner)
    $app = Start-TestApp -Runner $Runner
    $selection = Wait-Until { Get-AppDialog $app 'IDC_SCAN_DUPLICATES' } 'Startup target is cancelled without scanning any directory'
    Close-AppDialog $selection
    Wait-AppIdle $app
    Invoke-AppCommand $app 'ID_TOOLS_INSTALLED_APPS' -Unchecked
    $dialog = Wait-Until { Get-AppDialog $app 'IDC_IA_LIST' } 'Installed application window opens without a disk scan'
    Wait-InstalledAppsIdle $dialog
    return @{ App = $app; Dialog = $dialog }
}
function Test-InstalledApps {
    param($Context, $Case)
    $root = 'Software\WinDirStatTests\InstalledApps-' + [guid]::NewGuid().ToString('D')
    $fixture = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($root)
    try {
        $fixture.SetValue('WDSInstalledAppsFixture', 'v1', [Microsoft.Win32.RegistryValueKind]::String)
        foreach ($source in 'Machine32','Machine64','User32','User64','Msix') { $fixture.CreateSubKey($source).Dispose() }
    } finally { $fixture.Dispose() }
    $directory = Join-Path $Context.Fixture 'SyntheticInstall'
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $sentinel = Join-Path $directory 'keep-user-data.cache'
    [IO.File]::WriteAllText($sentinel, 'No directory scan or uninstall may change this synthetic payload.')
    $hash = (Get-FileHash -LiteralPath $sentinel).Hash
    New-InstalledAppRecord $root Machine64 large 'Large App' $directory -Publisher 'Vendor X' -Version '3.2' -SizeKiB 4096
    New-InstalledAppRecord $root Machine32 small 'Small App' -SizeKiB 1024
    New-InstalledAppRecord $root User32 shared 'Current App' -SizeKiB 512
    New-InstalledAppRecord $root User64 shared 'Current App' -SizeKiB 512
    New-InstalledAppRecord $root Machine64 bad 'Unknown Size' -BadSize
    New-InstalledAppRecord $root Msix package 'Packaged Tool' -SizeKiB 9999
    New-InstalledAppRecord $root Machine32 missing 'Missing Folder' (Join-Path $Context.Fixture 'DoesNotExist')
    New-InstalledAppRecord $root Machine64 unicode '中文工具' -Version '2.5'
    $runner = New-TestRunner @{ Options = @{ LanguageId = $Case.Language; AutoElevate = 0; UseSizeSuffixes = 0 } }
    $runner | Add-Member -NotePropertyName Environment -NotePropertyValue @{ WDS_INSTALLED_APPS_TEST_KEY = $root }
    $session = Start-InstalledAppsFixture $runner
    $dialog = $session.Dialog
    $app = $session.App
    $list = Get-DialogControl $dialog 'IDC_IA_LIST'
    Assert-Equal ([NativeListViewHelper]::GetItemCount($list)) 7 'Only synthetic apps appear; shared user registry views deduplicate'
    $names = @([NativeListViewHelper]::GetItemTexts($list))
    Assert-Sequence @($names | Select-Object -First 3) @('Large App','Small App','Current App') 'Known sizes sort numerically from largest to smallest'
    $sizes = @([NativeListViewHelper]::GetItemTexts($list, 3))
    Assert-True ($sizes[0] -match '4[, .]?194[, .]?304.*(estimate|估算)') 'Reported KiB becomes a labelled byte estimate'
    Assert-True ($sizes[3] -match 'Unknown|未知') 'Missing or malformed size is unknown, not zero'
    $packageRow = [array]::IndexOf($names, 'Packaged Tool')
    Assert-True ($sizes[$packageRow] -match 'Unknown|未知') 'MSIX size is not invented from a synthetic registry estimate'
    $notice = [TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_IA_NOTICE'))
    Assert-True ($notice -match 'overlap|重叠') 'Overlapping records are not added to a misleading total'
    Assert-True ($notice -match 'caches|缓存') 'Application registration is distinguished from user data and caches'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_IA_STATUS')) -match 'incomplete|不完整') 'Malformed source visibly marks inventory as partial'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_IA_LOCATE'))) 'No directory action without an explicit selected record'

    Invoke-CleanupButton $dialog 'IDC_IA_SORT'
    $ascending = @([NativeListViewHelper]::GetItemTexts($list))
    Assert-Sequence @($ascending | Select-Object -First 3) @('Current App','Small App','Large App') 'Ascending sorts known sizes while keeping unknown rows last'
    Set-InstalledAppsSearch $dialog 'vEnDoR x'
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($list)) @('Large App') 'Search matches publisher without case sensitivity'
    Assert-True ([NativeListViewHelper]::SelectSingleItem($list, 0)) 'Select explicit synthetic application'
    Wait-Until { [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_IA_LOCATE')) } 'Known local install path enables locate' | Out-Null
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_IA_DETAILS')).Contains($directory)) 'Full reported installation path is visible outside clipped columns'
    Invoke-CleanupButton $dialog 'IDC_IA_LOCATE'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_IA_STATUS')) -match 'simulated|模拟') 'Locate validates only the synthetic directory and simulates Explorer'
    Invoke-CleanupButton $dialog 'IDC_IA_WINDOWS'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_IA_STATUS')) -match 'simulated|模拟') 'Official uninstall page is simulated and never starts an uninstaller'
    Set-InstalledAppsSearch $dialog '3.2'
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($list)) @('Large App') 'Search includes displayed version'
    Set-InstalledAppsSearch $dialog '中文'
    Assert-Sequence @([NativeListViewHelper]::GetItemTexts($list)) @('中文工具') 'Unicode search is preserved'
    Assert-True ([NativeListViewHelper]::SelectSingleItem($list, 0)) 'Select unknown-location record'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $dialog 'IDC_IA_LOCATE'))) 'Unknown installation location cannot trigger a guessed path'
    Set-InstalledAppsSearch $dialog 'Missing Folder'
    [void][NativeListViewHelper]::SelectSingleItem($list, 0)
    Invoke-CleanupButton $dialog 'IDC_IA_LOCATE'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_IA_STATUS')) -match 'missing|缺失') 'A stale registered directory fails explicitly without fallback'
    Set-InstalledAppsSearch $dialog 'NEVER_EXECUTE'
    Assert-Equal ([NativeListViewHelper]::GetItemCount($list)) 0 'Uninstall commands never become searchable application metadata'
    Set-InstalledAppsSearch $dialog ''
    Invoke-CleanupButton $dialog 'IDC_IA_SORT'
    $bounds = [TestDesktop]::Rectangle($dialog)
    $work = [System.Windows.Forms.Screen]::FromHandle($dialog).WorkingArea
    Assert-True ($bounds[0] -ge $work.Left -and $bounds[1] -ge $work.Top -and $bounds[2] -le $work.Right -and $bounds[3] -le $work.Bottom) 'The complete application window fits the desktop working area'
    Save-Frame ([TestDesktop]::Capture($dialog)) (Join-Path $Context.Root 'installed-apps.png')
    foreach ($id in 'IDC_IA_REFRESH','IDC_IA_STOP','IDC_IA_LOCATE','IDC_IA_WINDOWS') {
        $button = [TestDesktop]::Rectangle((Get-DialogControl $dialog $id))
        Assert-True ($button[0] -ge $bounds[0] -and $button[2] -le $bounds[2] -and $button[3] -lt $bounds[3]) 'Actions remain within the dialog frame'
    }
    Invoke-CleanupButton $dialog 'IDC_IA_REFRESH'
    Invoke-CleanupButton $dialog 'IDC_IA_STOP'
    Wait-InstalledAppsIdle $dialog
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $dialog 'IDC_IA_STATUS')) -match 'cancelled|已取消') 'Cancellation completes visibly and does not block the GUI'
    Invoke-CleanupButton $dialog 'IDC_IA_REFRESH'
    Wait-InstalledAppsIdle $dialog
    Assert-Equal ([NativeListViewHelper]::GetItemCount($list)) 7 'Refresh retries the same explicit source after cancellation'
    Assert-Equal (Get-FileHash -LiteralPath $sentinel).Hash $hash 'All information actions preserve synthetic user data and cache content'
    Close-AppDialog $dialog
    Stop-TestApp $app

    $invalid = New-TestRunner @{ Options = @{ LanguageId = $Case.Language } }
    $invalid | Add-Member -NotePropertyName Environment -NotePropertyValue @{ WDS_INSTALLED_APPS_TEST_KEY = 'not-a-valid-test-root' }
    $session = Start-InstalledAppsFixture $invalid
    Assert-Equal ([NativeListViewHelper]::GetItemCount((Get-DialogControl $session.Dialog 'IDC_IA_LIST'))) 0 'Invalid fixture does not expose the real application list'
    Assert-True ([TestDesktop]::ControlText((Get-DialogControl $session.Dialog 'IDC_IA_STATUS')) -match 'Invalid|无效') 'Invalid synthetic source is explained'
    Assert-True (-not [TestDesktop]::IsWindowEnabled((Get-DialogControl $session.Dialog 'IDC_IA_WINDOWS'))) 'Invalid fixture cannot fall through to a real external action'
    Close-AppDialog $session.Dialog
    Stop-TestApp $session.App
}
Register-Scenario apps.inventory InstalledApps 'Synthetic installed apps: estimates, unknown, search, sorting and simulated official entry' `
    Test-InstalledApps -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 9 }
Register-Scenario apps.inventory.zh InstalledApps 'Chinese installed apps metadata and unknown-size explanations on synthetic sources' `
    Test-InstalledApps -Tags Desktop -Requires Windows,Ntfs,Desktop -Data @{ Language = 30724 }
