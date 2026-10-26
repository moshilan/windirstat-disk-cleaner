// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "InstalledAppsDlg.h"
#include "Cleanup/CleanupPreview.h"
#include "Finder.h"

namespace
{
    bool LocalDirectoryText(const std::wstring& path)
    {
        return path.size() > 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
            path[1] == L':' && path[2] == L'\\' && path.find_first_of(L"\r\n\t\"<>|?*") == std::wstring::npos &&
            path.find(L':', 2) == std::wstring::npos;
    }
}
CInstalledAppsDlg::CInstalledAppsDlg(WindowRef parent) : MessageTarget(IDD_INSTALLED_APPS, nullptr, parent) {}
CInstalledAppsDlg::~CInstalledAppsDlg()
{
    if (m_worker.joinable()) { m_worker.request_stop(); m_worker.join(); }
}
bool CInstalledAppsDlg::OnInitDialog()
{
    CLayoutDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);
    m_list.SubclassDlgItem(IDC_IA_LIST, this);
    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const std::array columns{ IDS_IA_NAME, IDS_IA_PUBLISHER, IDS_IA_VERSION, IDS_IA_SIZE, IDS_IA_LOCATION, IDS_IA_SOURCE };
    const std::array widths{ 220, 170, 110, 160, 300, 180 };
    for (int i = 0; i < static_cast<int>(columns.size()); ++i)
        m_list.InsertColumn(i, Localization::Lookup(columns[i]), i == 3 ? LVCFMT_RIGHT : LVCFMT_LEFT, widths[i]);
    DarkMode::AdjustControls(*this);
    if (DarkMode::IsDarkModeActive())
    {
        m_list.SetBkColor(DarkMode::SystemColor(COLOR_WINDOW));
        m_list.SetTextBkColor(DarkMode::SystemColor(COLOR_WINDOW));
        m_list.SetTextColor(DarkMode::SystemColor(COLOR_WINDOWTEXT));
    }
    m_layout.AddControl(IDC_IA_NOTICE, 0, 0, 1, 0);
    m_layout.AddControl(IDC_IA_SEARCH, 0, 0, 1, 0);
    m_layout.AddControl(IDC_IA_SORT, 1, 0, 0, 0);
    m_layout.AddControl(IDC_IA_LIST, 0, 0, 1, 1);
    for (auto id : { IDC_IA_DETAILS, IDC_IA_STATUS }) m_layout.AddControl(id, 0, 1, 1, 0);
    for (auto id : { IDC_IA_REFRESH, IDC_IA_STOP, IDC_IA_LOCATE, IDC_IA_WINDOWS }) m_layout.AddControl(id, 0, 1, 0, 0);
    m_layout.AddControl(IDCANCEL, 1, 1, 0, 0);
    m_layout.OnInitDialog(true);
    SetLastError(ERROR_SUCCESS);
    const auto length = GetEnvironmentVariableW(L"WDS_INSTALLED_APPS_TEST_KEY", nullptr, 0);
    m_fixture = length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    if (length != 0)
    {
        m_fixtureKey.resize(length);
        const auto copied = GetEnvironmentVariableW(L"WDS_INSTALLED_APPS_TEST_KEY", m_fixtureKey.data(), length);
        if (copied == 0 || copied >= length) m_fixtureKey.clear();
        else m_fixtureKey.resize(copied);
    }
    SetTimer(1, 100);
    OnRefresh();
    return true;
}
std::wstring CInstalledAppsDlg::SourceName(const Space::InstalledApps::Source source)
{
    using enum Space::InstalledApps::Source;
    switch (source)
    {
    case Machine32: return Localization::Lookup(IDS_IA_MACHINE32);
    case Machine64: return Localization::Lookup(IDS_IA_MACHINE64);
    case User32: case User64: return Localization::Lookup(IDS_IA_USER);
    default: return Localization::Lookup(IDS_IA_MSIX);
    }
}
const Space::InstalledApps::Item* CInstalledAppsDlg::Selected() const
{
    const int row = m_list.GetFirstSelectedIndex();
    return row >= 0 && static_cast<size_t>(row) < m_visible.size() ? &m_result.items[m_visible[row]] : nullptr;
}
void CInstalledAppsDlg::Controls()
{
    GetDlgItem(IDC_IA_REFRESH).EnableWindow(!m_busy);
    GetDlgItem(IDC_IA_STOP).EnableWindow(m_busy);
    GetDlgItem(IDC_IA_SORT).EnableWindow(!m_busy);
    const auto* item = Selected();
    GetDlgItem(IDC_IA_LOCATE).EnableWindow(!m_busy && item && LocalDirectoryText(item->installLocation));
    GetDlgItem(IDC_IA_WINDOWS).EnableWindow(!m_busy && !m_result.invalidFixture);
}
void CInstalledAppsDlg::OnRefresh()
{
    if (m_busy) return;
    m_busy = true;
    m_finished = false;
    m_error = false;
    m_list.SetItemCountEx(0);
    m_visible.clear();
    m_result = {};
    SetText(IDC_IA_DETAILS, L"");
    SetText(IDC_IA_STATUS, Localization::Lookup(IDS_IA_LOADING));
    Controls();
    m_worker = std::jthread([this](std::stop_token stop)
    {
        try { m_result = m_fixture ? Space::InstalledApps::LoadFixtureRegistry(m_fixtureKey, stop) : Space::InstalledApps::Load(stop); }
        catch (...) { m_error = true; m_result = {}; }
        m_finished.store(true);
    });
}
void CInstalledAppsDlg::OnStop()
{
    if (!m_busy) return;
    m_worker.request_stop();
    GetDlgItem(IDC_IA_STOP).EnableWindow(false);
    SetText(IDC_IA_STATUS, Localization::Lookup(IDS_CP_STOPPING));
}
void CInstalledAppsDlg::OnCancel()
{
    if (m_busy) { m_closePending = true; OnStop(); return; }
    ::KillTimer(Handle(), 1);
    CLayoutDialog::OnCancel();
}
void CInstalledAppsDlg::OnTimer(UINT_PTR)
{
    if (!m_busy || !m_finished.load()) return;
    const bool cancelled = m_worker.get_stop_token().stop_requested();
    m_worker.join();
    m_result.cancelled |= cancelled;
    m_busy = false;
    if (m_closePending) { OnCancel(); return; }
    Fill();
}
void CInstalledAppsDlg::Fill()
{
    if (m_busy) return;
    m_filling = true;
    m_list.SetItemCountEx(0);
    m_visible = Space::InstalledApps::Select(m_result.items, GetDlgItem(IDC_IA_SEARCH).GetText(), m_descending);
    m_list.SetItemCountEx(static_cast<int>(m_visible.size()));
    m_list.Invalidate();
    m_filling = false;
    bool incomplete = m_error;
    using enum Space::InstalledApps::State;
    for (const auto& source : m_result.sources) incomplete |= source.state != Complete;
    SetText(IDC_IA_STATUS, Localization::Format(IDS_IA_COUNTS, FormatCount(m_visible.size()), FormatCount(m_result.items.size())) +
        L" " + Localization::Lookup(m_result.invalidFixture ? IDS_IA_FIXTURE_INVALID : m_error ? IDS_IA_FAILED : m_result.cancelled ? IDS_IA_CANCELLED : incomplete ? IDS_IA_PARTIAL : IDS_IA_READY) +
        (m_fixture ? L" " + Localization::Lookup(IDS_IA_FIXTURE) : L""));
    SetText(IDC_IA_SORT, Localization::Lookup(m_descending ? IDS_IA_SORT_DESC : IDS_IA_SORT_ASC));
    Details();
}
void CInstalledAppsDlg::Details()
{
    if (m_busy || m_filling) return;
    std::wstring text;
    if (const auto* item = Selected())
    {
        const auto unknown = Localization::Lookup(IDS_IA_UNKNOWN);
        text = item->name + L"\r\n" + (item->installLocation.empty() ? unknown : item->installLocation) + L"\r\n" +
            SourceName(item->source) + L" | " + item->id + L"\r\n" + Localization::Lookup(IDS_IA_SIZE_NOTE);
    }
    else text = Localization::Lookup(IDS_IA_SELECT);
    if (m_error) text += L"\r\n" + Localization::Lookup(IDS_IA_FAILED);
    for (const auto& source : m_result.sources)
        if (!m_error && source.state != Space::InstalledApps::State::Complete)
            text += L"\r\n" + Localization::Format(IDS_IA_SOURCE_FAILURE, SourceName(source.source), source.error);
    SetText(IDC_IA_DETAILS, text);
    Controls();
}
void CInstalledAppsDlg::OnSearch() { if (!m_busy) Fill(); }
void CInstalledAppsDlg::OnSort() { if (!m_busy) { m_descending = !m_descending; Fill(); } }
void CInstalledAppsDlg::OnColumn(NMHDR* header, LRESULT* result)
{
    *result = 0;
    if (reinterpret_cast<NMLISTVIEW*>(header)->iSubItem == 3) OnSort();
}
void CInstalledAppsDlg::OnSelect(NMHDR*, LRESULT* result) { *result = 0; Details(); }
void CInstalledAppsDlg::OnDisplay(NMHDR* header, LRESULT* result)
{
    *result = 0;
    auto& cell = reinterpret_cast<NMLVDISPINFO*>(header)->item;
    if (!(cell.mask & LVIF_TEXT) || cell.iItem < 0 || static_cast<size_t>(cell.iItem) >= m_visible.size()) return;
    const auto& item = m_result.items[m_visible[cell.iItem]];
    switch (cell.iSubItem)
    {
    case 0: m_cell = item.name; break;
    case 1: m_cell = item.publisher; break;
    case 2: m_cell = item.version; break;
    case 3: m_cell = item.estimatedBytes ? Localization::Format(IDS_IA_ESTIMATE, FormatBytes(*item.estimatedBytes)) : Localization::Lookup(IDS_IA_UNKNOWN); break;
    case 4: m_cell = item.installLocation; break;
    case 5: m_cell = SourceName(item.source); break;
    default: m_cell.clear(); break;
    }
    if (m_cell.empty()) m_cell = Localization::Lookup(IDS_IA_UNKNOWN);
    cell.pszText = m_cell.data();
}
void CInstalledAppsDlg::OnLocate()
{
    const auto* item = Selected();
    if (m_busy || !item || !LocalDirectoryText(item->installLocation)) return;
    std::wstring path, error;
    const auto driveType = GetDriveTypeW(item->installLocation.substr(0, 3).c_str());
    if ((driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE) ||
        !Cleanup::NormalizeDirectory(item->installLocation, path, error) || !Cleanup::HasSafeAncestors(path))
    { SetText(IDC_IA_STATUS, Localization::Lookup(IDS_IA_LOCATION_FAILED)); return; }
    const auto attributes = GetFileAttributesW(Finder::MakeLongPathCompatible(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
    { SetText(IDC_IA_STATUS, Localization::Lookup(IDS_IA_LOCATION_FAILED)); return; }
    if (m_fixture) { SetText(IDC_IA_STATUS, Localization::Lookup(IDS_IA_LOCATE_TEST)); return; }
    const auto pidl = ILCreateFromPathW(path.c_str());
    const HRESULT opened = pidl ? SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0) : E_INVALIDARG;
    CoTaskMemFree(pidl);
    SetText(IDC_IA_STATUS, Localization::Lookup(SUCCEEDED(opened) ? IDS_IA_LOCATED : IDS_IA_LOCATION_FAILED));
}
void CInstalledAppsDlg::OnWindows()
{
    if (m_busy) return;
    if (m_fixture) { SetText(IDC_IA_STATUS, Localization::Lookup(IDS_IA_WINDOWS_TEST)); return; }
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    launch.hwnd = Handle();
    launch.lpVerb = L"open";
    launch.lpFile = L"ms-settings:appsfeatures";
    launch.nShow = SW_SHOWNORMAL;
    SetText(IDC_IA_STATUS, Localization::Lookup(ShellExecuteExW(&launch) ? IDS_IA_WINDOWS_REQUESTED : IDS_IA_WINDOWS_FAILED));
}
