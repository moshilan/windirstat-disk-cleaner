// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "CleanupPreviewDlg.h"
#include "CleanupPlanDlg.h"
#include "NuGetHttpCacheDlg.h"

namespace
{
    constexpr UINT_PTR CleanupTimer = 1;
    constexpr std::array RefreshPhases{ L"DataUs", L"FilterUs", L"SortUs", L"ControlsUs", L"SummaryUs", L"TotalUs", L"PaintUs" };
    std::uint64_t RefreshClock()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void RefreshTiming(const HWND window, const wchar_t* phase, const std::uint64_t elapsed)
    {
        const auto name = std::wstring(L"WDS.Cleanup.Refresh.") + phase;
        ::SetPropW(window, name.c_str(), reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(elapsed)));
    }
    LRESULT CALLBACK ProfileListPaint(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR owner)
    {
        if (message != WM_PAINT) return ::DefSubclassProc(window, message, wp, lp);
        const auto started = RefreshClock();
        const auto result = ::DefSubclassProc(window, message, wp, lp);
        const auto dialog = reinterpret_cast<HWND>(owner);
        const auto previous = reinterpret_cast<UINT_PTR>(::GetPropW(dialog, L"WDS.Cleanup.Refresh.PaintUs"));
        RefreshTiming(dialog, L"PaintUs", previous + RefreshClock() - started);
        return result;
    }
    void Cell(CListCtrl& list, const int row, const int column, const std::wstring& text)
    {
        LVITEMW item{};
        item.iSubItem = column;
        item.pszText = const_cast<wchar_t*>(text.c_str());
        ::SendMessageW(list.Handle(), LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
    }
    void Row(CListCtrl& list, const int row, const std::wstring& text)
    {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(text.c_str());
        ::SendMessageW(list.Handle(), LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    }
    bool IsRowChecked(const CListCtrl& list, const int row)
    {
        return (list.GetItemState(row, LVIS_STATEIMAGEMASK) >> 12) == 2;
    }
    void Check(CListCtrl& list, const int row, const bool checked)
    {
        list.SetItemState(row, INDEXTOSTATEIMAGEMASK(checked ? 2 : 1), LVIS_STATEIMAGEMASK);
    }
    std::wstring KnownFolder(const KNOWNFOLDERID& id)
    {
        PWSTR value = nullptr;
        if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value))) return {};
        std::wstring result(value);
        CoTaskMemFree(value);
        return result;
    }
    std::wstring CoverageName(const Cleanup::CoverageState state)
    {
        static constexpr std::array names{ IDS_CP_NOT_SCANNED, IDS_CP_DISABLED, IDS_CP_MISSING, IDS_CP_NO_MATCHES,
            IDS_CP_MATCHES, IDS_CP_SKIPPED, IDS_CP_FAILED, IDS_CP_PARTIAL };
        return Localization::Lookup(names[static_cast<size_t>(state)]);
    }
    std::wstring RuleScope(const Cleanup::Rule& rule)
    {
        if (rule.category == Cleanup::Category::Temporary) return rule.relativeRoot + L"\\**\\*.tmp; *.temp";
        switch (rule.layout)
        {
        case Cleanup::Layout::ChromiumProfiles: return rule.relativeRoot + L"\\Default|Profile <n>\\Cache\\Cache_Data\\**";
        case Cleanup::Layout::FirefoxProfiles: return rule.relativeRoot + L"\\<profile>\\cache2\\entries\\**";
        case Cleanup::Layout::CommitDirectories: return rule.relativeRoot + L"\\<40 hex commit>\\**";
        default: return rule.relativeRoot + L"\\**";
        }
    }
    std::wstring RiskName(const Cleanup::Risk risk, const Cleanup::Assessment assessment)
    {
        const auto level = risk == Cleanup::Risk::Low ? IDS_CP_LOW : risk == Cleanup::Risk::High ? IDS_CP_HIGH : IDS_CP_MEDIUM;
        return Localization::Lookup(level) + L" | " + Localization::Lookup(assessment == Cleanup::Assessment::RegenerableCache ?
            IDS_CP_ASSESS_REGENERABLE : IDS_CP_ASSESS_MANUAL);
    }
    std::wstring ProtectedScope(const Cleanup::Category category)
    {
        switch (category)
        {
        case Cleanup::Category::Temporary: return Localization::Lookup(IDS_CP_PROTECTED_TEMP);
        case Cleanup::Category::VSCode: return Localization::Lookup(IDS_CP_PROTECTED_VSCODE);
        case Cleanup::Category::Npm: return Localization::Lookup(IDS_CP_PROTECTED_NPM);
        case Cleanup::Category::NuGet: return Localization::Lookup(IDS_CP_PROTECTED_NUGET);
        case Cleanup::Category::Pip: return Localization::Lookup(IDS_CP_PROTECTED_PIP);
        default: return Localization::Lookup(IDS_CP_PROTECTED_BROWSER);
        }
    }
    std::wstring ExclusionLines(const std::vector<std::wstring>& directories)
    {
        std::wstring text;
        for (const auto& directory : directories)
        {
            if (!text.empty()) text += L"\r\n";
            text += directory;
        }
        return text;
    }
}

CCleanupPreviewDlg::CCleanupPreviewDlg(WindowRef parent) : MessageTarget(IDD_CLEANUP_PREVIEW, nullptr, parent) {}
CCleanupPreviewDlg::~CCleanupPreviewDlg()
{
    if (m_worker.joinable()) { m_worker.request_stop(); m_worker.join(); }
    if (m_fixture)
    {
        ::RemoveWindowSubclass(m_files.Handle(), ProfileListPaint, 1);
        for (const auto* phase : RefreshPhases)
            ::RemovePropW(Handle(), (std::wstring(L"WDS.Cleanup.Refresh.") + phase).c_str());
    }
}
std::wstring CCleanupPreviewDlg::CategoryName(const Cleanup::Category category)
{
    static constexpr std::array names{ IDS_CP_TEMP, IDS_CP_CHROME, IDS_CP_EDGE, IDS_CP_FIREFOX, IDS_CP_PIP,
        IDS_CP_BRAVE, IDS_CP_VIVALDI, IDS_CP_VSCODE, IDS_CP_NPM, IDS_CP_NUGET };
    return Localization::Lookup(names[static_cast<size_t>(category)]);
}

bool CCleanupPreviewDlg::ResolveOptions()
{
    const DWORD size = GetEnvironmentVariableW(L"WDS_CLEANUP_TEST_ROOT", nullptr, 0);
    if (size != 0)
    {
        std::wstring fixture(size, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"WDS_CLEANUP_TEST_ROOT", fixture.data(), size);
        if (copied == 0 || copied >= size) return false;
        fixture.resize(copied);
        std::wstring error;
        if (!Cleanup::ResolveTestFixture(fixture, m_options.localAppData, error))
        {
            SetText(IDC_CP_ISSUES, Localization::Lookup(IDS_CP_FIXTURE_ERROR));
            return false;
        }
        m_options.roamingAppData = m_options.localAppData.substr(0, m_options.localAppData.find_last_of(L'\\')) + L"\\RoamingAppData";
        m_fixture = true;
        return true;
    }
    m_options.localAppData = KnownFolder(FOLDERID_LocalAppData);
    m_options.roamingAppData = KnownFolder(FOLDERID_RoamingAppData);
    if (m_options.localAppData.empty()) return false;
    for (const auto* id : { &FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
        &FOLDERID_ProgramData, &FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads,
        &FOLDERID_Pictures, &FOLDERID_Videos, &FOLDERID_Music, &FOLDERID_SkyDrive })
    {
        if (auto path = KnownFolder(*id); !path.empty()) m_options.protectedDirectories.push_back(std::move(path));
    }
    return true;
}

std::wstring CCleanupPreviewDlg::RuleDetails(const Cleanup::Rule& rule) const
{
    const auto& anchor = rule.anchor == Cleanup::Anchor::LocalAppData ? m_options.localAppData : m_options.roamingAppData;
    return anchor + L"\\" + RuleScope(rule) + L"\r\n" +
        Localization::Format(IDS_CP_RULE_DETAILS, rule.id, rule.version, RuleScope(rule), rule.minimumAgeDays) +
        L"\r\n" + Localization::Lookup(rule.reasonKey) + L"\r\n" + RiskName(rule.risk, rule.assessment) +
        L"\r\n" + ProtectedScope(rule.category);
}

void CCleanupPreviewDlg::LoadExclusions()
{
    std::vector<std::wstring> directories;
    std::wstring error;
    m_exclusionsCorrupt = !Cleanup::DecodeExclusions(COptions::CleanupPreviewExclusions.Obj(), directories, error);
    m_internalChange = true;
    m_savedExclusionsText = ExclusionLines(directories);
    m_exclusions.SetText(m_exclusionsCorrupt ? Localization::Lookup(IDS_CP_EXCLUSIONS_CORRUPT) : m_savedExclusionsText);
    m_internalChange = false;
    if (m_exclusionsCorrupt) SetText(IDC_CP_ISSUES, Localization::Lookup(IDS_CP_EXCLUSIONS_CORRUPT));
}

bool CCleanupPreviewDlg::SaveExclusions()
{
    const auto text = m_exclusions.GetText();
    if (!m_exclusionsCorrupt && text == m_savedExclusionsText) return true;
    std::vector<std::wstring> directories;
    std::wstring error;
    if (!Cleanup::ParseExclusions(text, directories, error))
    {
        SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_EXCLUSIONS_INVALID));
        SetText(IDC_CP_ISSUES, text + L"\r\n" + Localization::Lookup(IDS_CP_EXCLUSIONS_INVALID));
        return false;
    }
    const auto encoded = Cleanup::EncodeExclusions(directories);
    const std::wstring previous = COptions::CleanupPreviewExclusions.Obj();
    COptions::CleanupPreviewExclusions = encoded;
    COptions::CleanupPreviewExclusions.WritePersistedProperty();
    COptions::CleanupPreviewExclusions = std::wstring(L"unreadable-exclusion-setting");
    COptions::CleanupPreviewExclusions.ReadPersistedProperty();
    if (COptions::CleanupPreviewExclusions.Obj() != encoded)
    {
        COptions::CleanupPreviewExclusions = previous;
        SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_EXCLUSIONS_SAVE_FAILED));
        SetText(IDC_CP_ISSUES, Localization::Lookup(IDS_CP_EXCLUSIONS_SAVE_FAILED));
        return false;
    }
    m_savedExclusionsText = text;
    m_exclusionsCorrupt = false;
    SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_EXCLUSIONS_SAVED));
    return true;
}

bool CCleanupPreviewDlg::OnInitDialog()
{
    CLayoutDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);
    m_categories.SubclassDlgItem(IDC_CP_CATEGORIES, this);
    m_files.SubclassDlgItem(IDC_CP_FILES, this);
    m_exclusions.SubclassDlgItem(IDC_CP_EXCLUSIONS, this);
    m_progressBar.SubclassDlgItem(IDC_CP_PROGRESS, this);
    for (auto* list : { &m_categories, &m_files })
        list->SetExtendedStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ::SendMessageW(m_files.Handle(), LVM_SETCALLBACKMASK, LVIS_STATEIMAGEMASK, 0);
    m_categories.InsertColumn(0, Localization::Lookup(IDS_CP_CATEGORY), LVCFMT_LEFT, 210);
    m_categories.InsertColumn(1, Localization::Lookup(IDS_CP_COUNT), LVCFMT_RIGHT, 70);
    m_categories.InsertColumn(2, Localization::Lookup(IDS_CP_ALLOCATED), LVCFMT_RIGHT, 110);
    m_categories.InsertColumn(3, Localization::Lookup(IDS_CP_RISK), LVCFMT_LEFT, 75);
    m_categories.InsertColumn(4, Localization::Lookup(IDS_CP_COVERAGE), LVCFMT_LEFT, 270);
    const std::array widths{ 340, 150, 120, 150, 350, 110 };
    const std::array columns{ IDS_CP_PATH, IDS_CP_CATEGORY, IDS_CP_ALLOCATED, IDS_CP_RULE, IDS_CP_REASON, IDS_CP_RISK };
    for (int i = 0; i < static_cast<int>(columns.size()); ++i)
        m_files.InsertColumn(i, Localization::Lookup(columns[i]), LVCFMT_LEFT, widths[i]);
    m_internalChange = true;
    for (size_t i = 0; i < Cleanup::CategoryCount; ++i)
    {
        Row(m_categories, static_cast<int>(i), CategoryName(static_cast<Cleanup::Category>(i)));
        Check(m_categories, static_cast<int>(i), true);
        Cell(m_categories, static_cast<int>(i), 1, L"0");
        Cell(m_categories, static_cast<int>(i), 2, FormatBytes(0));
        const auto primary = std::ranges::find(Cleanup::Rules(), static_cast<Cleanup::Category>(i), &Cleanup::Rule::category);
        Cell(m_categories, static_cast<int>(i), 3, RiskName(primary->risk, primary->assessment));
        Cell(m_categories, static_cast<int>(i), 4, CoverageName(Cleanup::CoverageState::NotScanned));
    }
    m_internalChange = false;
    DarkMode::AdjustControls(*this);
    if (DarkMode::IsDarkModeActive()) for (auto* list : { &m_categories, &m_files })
    {
        list->SetBkColor(DarkMode::SystemColor(COLOR_WINDOW));
        list->SetTextBkColor(DarkMode::SystemColor(COLOR_WINDOW));
        list->SetTextColor(DarkMode::SystemColor(COLOR_WINDOWTEXT));
    }
    m_layout.AddControl(IDC_CP_NOTICE, 0, 0, 1, 0);
    m_layout.AddControl(IDC_CP_WINDOWS_RECOMMENDATIONS, 0, 0, 0, 0);
    m_layout.AddControl(IDC_CP_WINDOWS_DISK, 0, 0, 0, 0);
    m_layout.AddControl(IDC_CP_NUGET, 0, 0, 0, 0);
    m_layout.AddControl(IDC_CP_PIP, 0, 0, 0, 0);
    m_layout.AddControl(IDC_CP_NPM, 0, 0, 1, 0);
    m_layout.AddControl(IDC_CP_WINDOWS_NOTICE, 0, 0, 1, 0);
    m_layout.AddControl(IDC_CP_CATEGORIES, 0, 0, 1, 0);
    m_layout.AddControl(IDC_CP_FILES, 0, 0, 1, 1);
    for (const auto id : { IDC_CP_DETAILS, IDC_CP_EXCLUSIONS, IDC_CP_ISSUES, IDC_CP_STATUS, IDC_CP_TOTALS, IDC_CP_PROGRESS })
        m_layout.AddControl(id, 0, 1, 1, 0);
    m_layout.AddControl(IDC_CP_ADD_EXCLUSION, 1, 1, 0, 0);
    m_layout.AddControl(IDC_CP_SCAN, 0, 1, 0, 0);
    m_layout.AddControl(IDC_CP_CANCEL, 0, 1, 0, 0);
    m_layout.AddControl(IDC_CP_SELECT_ALL, 0, 1, 0, 0);
    m_layout.AddControl(IDC_CP_SELECT_NONE, 0, 1, 0, 0);
    m_layout.AddControl(IDC_CP_SORT_SIZE, 0, 0, 0, 0);
    m_layout.AddControl(IDC_CP_HIDE_ZERO, 0, 0, 0, 0);
    m_layout.AddControl(IDC_CP_PLAN, 1, 0, 0, 0);
    m_layout.AddControl(IDCANCEL, 1, 1, 0, 0);
    m_layout.OnInitDialog(true);
    const auto window = GetWindowRect();
    const auto client = GetClientRect();
    const auto categories = GetChildWindowRect(m_categories.Handle());
    const auto files = GetChildWindowRect(m_files.Handle());
    const auto details = GetChildWindowRect(GetDlgItem(IDC_CP_DETAILS).Handle());
    const auto lastButton = GetChildWindowRect(GetDlgItem(IDC_CP_SELECT_NONE).Handle());
    const auto closeButton = GetChildWindowRect(GetDlgItem(IDCANCEL).Handle());
    m_minimumSize = CSize(lastButton.right + closeButton.Width() + categories.left * 2 + window.Width() - client.Width(),
        window.Height() - categories.Height() - files.Height() - details.Height() + ScaleForDpi(65 + 64 + 32));
    const auto npmButton = GetChildWindowRect(GetDlgItem(IDC_CP_NPM).Handle());
    const auto pipButton = GetChildWindowRect(GetDlgItem(IDC_CP_PIP).Handle());
    m_minimumSize.cx = std::max(m_minimumSize.cx,
        npmButton.left + pipButton.Width() + categories.left + window.Width() - client.Width());
    m_layoutReady = true;
    MONITORINFO monitor{ sizeof(monitor) };
    if (::GetMonitorInfoW(::MonitorFromWindow(Handle(), MONITOR_DEFAULTTONEAREST), &monitor))
    {
        const auto rect = GetWindowRect();
        const int width = std::min(rect.Width(), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left - 16));
        const int height = std::min(rect.Height(), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top - 16));
        ::SetWindowPos(Handle(), nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
            width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    m_optionsValid = ResolveOptions();
    if (m_fixture) ::SetWindowSubclass(m_files.Handle(), ProfileListPaint, 1, reinterpret_cast<DWORD_PTR>(Handle()));
    if (m_fixture) SetText(IDC_CP_NOTICE, Localization::Lookup(IDS_CP_FIXTURE_NOTICE));
    LoadExclusions();
    SetText(IDC_CP_STATUS, Localization::Lookup(!m_optionsValid ? IDS_CP_UNAVAILABLE :
        m_exclusionsCorrupt ? IDS_CP_EXCLUSIONS_CORRUPT : IDS_CP_READY));
    SetText(IDC_CP_DETAILS, Localization::Lookup(IDS_CP_SELECT_HINT));
    SetScanning(false);
    UpdateSummary();
    SetTimer(CleanupTimer, 100);
    return true;
}

void CCleanupPreviewDlg::OnGetMinMaxInfo(MINMAXINFO* info)
{
    CLayoutDialog::OnGetMinMaxInfo(info);
    if (m_minimumSize.cx > 0) info->ptMinTrackSize = { m_minimumSize.cx, m_minimumSize.cy };
    MONITORINFO monitor{ sizeof(monitor) };
    if (::GetMonitorInfoW(::MonitorFromWindow(Handle(), MONITOR_DEFAULTTONEAREST), &monitor))
    {
        info->ptMinTrackSize.x = std::min(info->ptMinTrackSize.x, monitor.rcWork.right - monitor.rcWork.left - 16);
        info->ptMinTrackSize.y = std::min(info->ptMinTrackSize.y, monitor.rcWork.bottom - monitor.rcWork.top - 16);
    }
}

void CCleanupPreviewDlg::OnSize(const UINT type, const int width, const int height)
{
    CLayoutDialog::OnSize(type, width, height);
    if (m_layoutReady && type != SIZE_MINIMIZED) LayoutPanes();
}

void CCleanupPreviewDlg::LayoutPanes()
{
    // The existing dialog layout restores baseline rectangles on every resize.
    // Apply user offsets afterwards, keeping every pane usable.
    auto categories = GetChildWindowRect(m_categories.Handle());
    auto files = GetChildWindowRect(m_files.Handle());
    auto details = GetChildWindowRect(GetDlgItem(IDC_CP_DETAILS).Handle());
    auto exclusions = GetChildWindowRect(m_exclusions.Handle());
    auto addExclusion = GetChildWindowRect(GetDlgItem(IDC_CP_ADD_EXCLUSION).Handle());
    auto issues = GetChildWindowRect(GetDlgItem(IDC_CP_ISSUES).Handle());
    const int minimum = ScaleForDpi(32);
    const int fileMinimum = ScaleForDpi(64);
    const int categoryMinimum = ScaleForDpi(65);
    m_issuesDelta = std::clamp(m_issuesDelta, minimum - issues.Height(),
        std::max(minimum - issues.Height(), categories.Height() + files.Height() + details.Height() - categoryMinimum - fileMinimum - minimum));
    files.bottom -= m_issuesDelta;
    details.Offset(0, -m_issuesDelta);
    exclusions.Offset(0, -m_issuesDelta);
    addExclusion.Offset(0, -m_issuesDelta);
    issues.top -= m_issuesDelta;
    m_categoryDelta = std::clamp(m_categoryDelta, categoryMinimum - categories.Height(),
        std::max(categoryMinimum - categories.Height(), files.Height() + details.Height() - fileMinimum - minimum));
    m_detailDelta = std::clamp(m_detailDelta, minimum - details.Height(),
        std::max(minimum - details.Height(), files.Height() - m_categoryDelta - fileMinimum));
    categories.bottom += m_categoryDelta;
    files.top += m_categoryDelta;
    files.bottom -= m_detailDelta;
    details.top -= m_detailDelta;
    m_categories.MoveWindow(categories);
    m_files.MoveWindow(files);
    GetDlgItem(IDC_CP_DETAILS).MoveWindow(details);
    m_exclusions.MoveWindow(exclusions);
    GetDlgItem(IDC_CP_ADD_EXCLUSION).MoveWindow(addExclusion);
    GetDlgItem(IDC_CP_ISSUES).MoveWindow(issues);
    for (const auto id : { IDC_CP_SORT_SIZE, IDC_CP_HIDE_ZERO, IDC_CP_PLAN })
    {
        auto rect = GetChildWindowRect(GetDlgItem(id).Handle());
        rect.Offset(0, m_categoryDelta);
        GetDlgItem(id).MoveWindow(rect);
    }
}

void CCleanupPreviewDlg::OnPaint()
{
    CPaintDC dc(this);
    if (!m_layoutReady) return;
    const auto categories = GetChildWindowRect(m_categories.Handle());
    const auto files = GetChildWindowRect(m_files.Handle());
    const auto details = GetChildWindowRect(GetDlgItem(IDC_CP_DETAILS).Handle());
    const auto toolbar = GetChildWindowRect(GetDlgItem(IDC_CP_SORT_SIZE).Handle());
    const auto exclusions = GetChildWindowRect(m_exclusions.Handle());
    const auto issues = GetChildWindowRect(GetDlgItem(IDC_CP_ISSUES).Handle());
    const CPen pen(PS_SOLID, 1, DarkMode::SystemColor(COLOR_3DSHADOW));
    const GdiObjectSelection selection(&dc, &pen);
    for (const int y : { (categories.bottom + toolbar.top) / 2, (files.bottom + details.top) / 2,
        (exclusions.bottom + issues.top) / 2 })
    {
        dc.MoveTo(files.left, y);
        dc.LineTo(files.right, y);
    }
}

int CCleanupPreviewDlg::SplitterAt(const CPoint point) const
{
    const auto categories = GetChildWindowRect(m_categories.Handle());
    const auto files = GetChildWindowRect(m_files.Handle());
    const auto details = GetChildWindowRect(GetDlgItem(IDC_CP_DETAILS).Handle());
    const auto toolbar = GetChildWindowRect(GetDlgItem(IDC_CP_SORT_SIZE).Handle());
    const auto exclusions = GetChildWindowRect(m_exclusions.Handle());
    const auto issues = GetChildWindowRect(GetDlgItem(IDC_CP_ISSUES).Handle());
    if (point.x < files.left || point.x >= files.right) return 0;
    if (point.y >= categories.bottom && point.y < toolbar.top) return 1;
    if (point.y >= files.bottom && point.y < details.top) return 2;
    if (point.y >= exclusions.bottom && point.y < issues.top) return 3;
    return 0;
}

void CCleanupPreviewDlg::OnLButtonDown(UINT, const CPoint point)
{
    if ((m_dragging = SplitterAt(point)) != 0)
    {
        m_dragY = point.y;
        ::SetCapture(Handle());
    }
    else CallDefaultHandler();
}
void CCleanupPreviewDlg::OnMouseMove(UINT, const CPoint point)
{
    if (!m_dragging || ::GetCapture() != Handle()) { CallDefaultHandler(); return; }
    const int delta = point.y - m_dragY;
    m_dragY = point.y;
    if (m_dragging == 1) m_categoryDelta += delta;
    else if (m_dragging == 2) m_detailDelta -= delta;
    else m_issuesDelta -= delta;
    m_layout.OnSize();
    LayoutPanes();
}
void CCleanupPreviewDlg::OnLButtonUp(UINT, CPoint)
{
    if (m_dragging && ::GetCapture() == Handle()) ::ReleaseCapture();
    else CallDefaultHandler();
    m_dragging = 0;
}
void CCleanupPreviewDlg::OnCaptureChanged(WindowRef) { m_dragging = 0; }
bool CCleanupPreviewDlg::OnSetCursor(const WindowRef window, const UINT hit, UINT)
{
    if (hit == HTCLIENT && window.Handle() == Handle())
    {
        const auto point = GetClientCursorPos();
        if (point && (m_dragging || SplitterAt(*point)))
        {
            ::SetCursor(::LoadCursorW(nullptr, IDC_SIZENS));
            return true;
        }
    }
    return static_cast<bool>(CallDefaultHandler());
}

void CCleanupPreviewDlg::FillFiles()
{
    const auto started = RefreshClock();
    if (m_fixture) RefreshTiming(Handle(), L"PaintUs", 0);
    const int focused = m_files.GetFirstSelectedIndex();
    const size_t focus = focused >= 0 && static_cast<size_t>(focused) < m_visible.size() ?
        m_visible[focused] : m_result.candidates.size();
    m_internalChange = true;
    std::uint64_t dataUs = RefreshClock() - started, controlsUs = 0;
    auto phase = RefreshClock();
    m_files.SetRedraw(false);
    m_files.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    controlsUs += RefreshClock() - phase;
    phase = RefreshClock();
    m_visible.clear();
    for (size_t i = 0; i < m_result.candidates.size(); ++i)
        if (!m_hideEmpty || m_result.candidates[i].logicalBytes != 0) m_visible.push_back(i);
    const auto filterUs = RefreshClock() - phase;
    phase = RefreshClock();
    if (m_sortSize) std::stable_sort(m_visible.begin(), m_visible.end(), [this](size_t a, size_t b)
    {
        const auto left = m_result.candidates[a].allocatedBytes, right = m_result.candidates[b].allocatedBytes;
        return m_descending ? left > right : left < right;
    });
    const auto sortUs = RefreshClock() - phase;
    phase = RefreshClock();
    // The control keeps only row count and focus. Text and check states come from
    // the existing candidate identity mapping, including rows restored by a filter.
    m_files.SetItemCountEx(static_cast<int>(m_visible.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (const auto position = std::ranges::find(m_visible, focus); position != m_visible.end())
        m_files.SetItemState(static_cast<int>(position - m_visible.begin()), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    else SetText(IDC_CP_DETAILS, Localization::Lookup(IDS_CP_SELECT_HINT));
    m_files.SetRedraw(true);
    m_files.Invalidate();
    controlsUs += RefreshClock() - phase;
    phase = RefreshClock();
    m_internalChange = false;
    SetText(IDC_CP_SORT_SIZE, Localization::Lookup(m_sortSize && m_descending ? IDS_CP_SORT_ASC : IDS_CP_SORT_DESC));
    UpdateSummary();
    if (m_fixture)
    {
        RefreshTiming(Handle(), L"DataUs", dataUs);
        RefreshTiming(Handle(), L"FilterUs", filterUs);
        RefreshTiming(Handle(), L"SortUs", sortUs);
        RefreshTiming(Handle(), L"ControlsUs", controlsUs);
        RefreshTiming(Handle(), L"SummaryUs", RefreshClock() - phase);
        RefreshTiming(Handle(), L"TotalUs", RefreshClock() - started);
    }
}
void CCleanupPreviewDlg::OnSortSize()
{
    if (m_scanning || !m_valid) return;
    if (m_sortSize) m_descending = !m_descending;
    else { m_sortSize = true; m_descending = true; }
    FillFiles();
}
void CCleanupPreviewDlg::OnColumnClick(NMHDR* header, LRESULT* result)
{
    *result = 0;
    if (reinterpret_cast<NMLISTVIEW*>(header)->iSubItem == 2) OnSortSize();
}
void CCleanupPreviewDlg::OnFilter()
{
    if (m_scanning || !m_valid) return;
    m_hideEmpty = ::IsDlgButtonChecked(Handle(), IDC_CP_HIDE_ZERO) == BST_CHECKED;
    FillFiles();
    GetDlgItem(IDC_CP_HIDE_ZERO).Invalidate();
}

void CCleanupPreviewDlg::SetScanning(const bool scanning)
{
    m_scanning = scanning;
    for (const auto id : { IDC_CP_CATEGORIES, IDC_CP_EXCLUSIONS, IDC_CP_ADD_EXCLUSION }) GetDlgItem(id).EnableWindow(!scanning && m_optionsValid);
    GetDlgItem(IDC_CP_SCAN).EnableWindow(!scanning && m_optionsValid);
    GetDlgItem(IDC_CP_CANCEL).EnableWindow(scanning);
    for (const auto id : { IDC_CP_WINDOWS_RECOMMENDATIONS, IDC_CP_WINDOWS_DISK, IDC_CP_NUGET, IDC_CP_PIP, IDC_CP_NPM }) GetDlgItem(id).EnableWindow(!scanning);
    GetDlgItem(IDC_CP_PLAN).EnableWindow(!scanning && m_valid && !m_result.incomplete && !m_result.cancelled &&
        !m_result.limitReached && m_result.failures == 0 && std::ranges::any_of(m_selected, [](bool value) { return value; }));
    for (const auto id : { IDC_CP_FILES, IDC_CP_SELECT_ALL, IDC_CP_SELECT_NONE, IDC_CP_SORT_SIZE, IDC_CP_HIDE_ZERO }) GetDlgItem(id).EnableWindow(!scanning && m_valid);
    m_progressBar.SetMarquee(scanning, 30);
}

void CCleanupPreviewDlg::InvalidatePreview()
{
    if (m_scanning || m_internalChange) return;
    m_internalChange = true;
    m_files.SetItemCountEx(0);
    m_result = {};
    m_visible.clear();
    m_selected.clear();
    m_valid = false;
    for (size_t i = 0; i < Cleanup::CategoryCount; ++i)
    {
        Cell(m_categories, static_cast<int>(i), 1, L"0");
        Cell(m_categories, static_cast<int>(i), 2, FormatBytes(0));
        Cell(m_categories, static_cast<int>(i), 4, Localization::Lookup(IDS_CP_NOT_SCANNED));
    }
    SetText(IDC_CP_DETAILS, L"");
    SetText(IDC_CP_ISSUES, L"");
    SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_READY));
    m_internalChange = false;
    SetScanning(false);
    UpdateSummary();
}
void CCleanupPreviewDlg::OnConfigurationChanged()
{
    if (m_internalChange || m_scanning) return;
    InvalidatePreview();
    if (m_optionsValid) SaveExclusions();
}
void CCleanupPreviewDlg::OnCategoryChanged(NMHDR* header, LRESULT* result)
{
    *result = 0;
    const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
    if ((change->uChanged & LVIF_STATE) && ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK)) InvalidatePreview();
    else if (!m_scanning && !m_internalChange && change->iItem >= 0 && (change->uNewState & LVIS_SELECTED))
    {
        const auto index = static_cast<size_t>(change->iItem);
        std::wstring details = CoverageName(m_result.coverage[index].state);
        for (const auto& rule : Cleanup::Rules()) if (static_cast<size_t>(rule.category) == index)
            details += L"\r\n" + RuleDetails(rule);
        SetText(IDC_CP_DETAILS, details);
    }
}

void CCleanupPreviewDlg::OnScan()
{
    if (m_scanning || !m_optionsValid) return;
    Cleanup::ScanOptions options = m_options;
    options.maximumCandidates = 10000;
    bool enabled = false;
    for (size_t i = 0; i < options.enabled.size(); ++i) enabled |= options.enabled[i] = IsRowChecked(m_categories, static_cast<int>(i));
    if (!enabled) { SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_CHOOSE)); return; }
    std::wstring error;
    if (!Cleanup::ParseExclusions(m_exclusions.GetText(), options.excludedDirectories, error))
    {
        SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_INVALID_EXCLUSION));
        SetText(IDC_CP_ISSUES, Localization::Lookup(IDS_CP_EXCLUSIONS_INVALID));
        return;
    }
    if (!SaveExclusions()) return;
    InvalidatePreview();
    m_scanOptions = options;
    m_started = GetTickCount64();
    m_finished = false;
    { std::lock_guard lock(m_mutex); m_progress = {}; }
    SetScanning(true);
    SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_SCANNING));
    m_worker = std::jthread([this, options = std::move(options)](std::stop_token token)
    {
        Cleanup::ScanResult scanned;
        try
        {
            scanned = Cleanup::Scan(options, token, [this](const Cleanup::Progress& progress)
            {
                std::lock_guard lock(m_mutex);
                m_progress = progress;
            });
        }
        catch (...)
        {
            scanned.incomplete = true;
            scanned.failures = 1;
            scanned.warnings.push_back({ L"", L"IDS_CP_SCAN_ERROR", true });
            for (size_t i = 0; i < Cleanup::CategoryCount; ++i)
                scanned.coverage[i].state = options.enabled[i] ? Cleanup::CoverageState::Failed : Cleanup::CoverageState::Disabled;
        }
        { std::lock_guard lock(m_mutex); m_result = std::move(scanned); }
        m_finished.store(true);
    });
}
void CCleanupPreviewDlg::OnStop()
{
    if (!m_scanning) return;
    m_worker.request_stop();
    GetDlgItem(IDC_CP_CANCEL).EnableWindow(false);
    SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_STOPPING));
}
void CCleanupPreviewDlg::OnCancel()
{
    if (m_scanning)
    {
        m_closePending = true;
        OnStop();
        return;
    }
    if (m_optionsValid) SaveExclusions();
    ::KillTimer(Handle(), CleanupTimer);
    CLayoutDialog::OnCancel();
}
void CCleanupPreviewDlg::OnTimer(UINT_PTR id)
{
    if (id != CleanupTimer || !m_scanning) return;
    if (!m_finished.load())
    {
        Cleanup::Progress progress;
        { std::lock_guard lock(m_mutex); progress = m_progress; }
        if (!m_worker.get_stop_token().stop_requested()) SetText(IDC_CP_STATUS, Localization::Format(IDS_CP_PROGRESS,
            CategoryName(progress.category), FormatCount(progress.checkedFiles), FormatCount(progress.candidates),
            FormatCount((GetTickCount64() - m_started) / 1000)));
        SetText(IDC_CP_DETAILS, progress.currentPath);
        return;
    }
    if (m_worker.joinable()) m_worker.join();
    m_valid = true;
    SetScanning(false);
    if (m_closePending) { OnCancel(); return; }
    FillResult();
}

void CCleanupPreviewDlg::FillResult()
{
    m_selected.assign(m_result.candidates.size(), false);
    FillFiles();
    m_internalChange = true;
    for (size_t i = 0; i < Cleanup::CategoryCount; ++i)
    {
        Cell(m_categories, static_cast<int>(i), 1, FormatCount(m_result.categories[i].files));
        Cell(m_categories, static_cast<int>(i), 2, FormatBytes(m_result.categories[i].allocatedBytes));
        Cell(m_categories, static_cast<int>(i), 4, CoverageName(m_result.coverage[i].state));
    }
    std::wstring issues;
    for (const auto& warning : m_result.warnings)
        issues += (warning.path.empty() ? L"" : warning.path + L": ") +
            (warning.resourceKey ? Localization::Lookup(warning.message) : warning.message) + L"\r\n";
    for (size_t i = 0; i < Cleanup::CategoryCount; ++i)
    {
        const auto& item = m_result.coverage[i];
        issues += CategoryName(static_cast<Cleanup::Category>(i)) + L": " + CoverageName(item.state) + L"\r\n" +
            Localization::Format(IDS_CP_COVERAGE_COUNTS, FormatCount(item.rootsFound), FormatCount(item.missingRoots),
                FormatCount(item.checkedFiles), FormatCount(item.failures), FormatCount(item.skipped)) + L"\r\n";
    }
    const std::array reasons{ IDS_CP_SKIP_RECENT, IDS_CP_SKIP_PATTERN, IDS_CP_SKIP_EXCLUDED, IDS_CP_SKIP_UNSAFE,
        IDS_CP_SKIP_HARDLINK, IDS_CP_SKIP_STREAMS, IDS_CP_SKIP_METADATA, IDS_CP_SKIP_DUPLICATE };
    for (size_t i = 0; i < reasons.size(); ++i) if (m_result.skipped[i] != 0)
        issues += Localization::Lookup(reasons[i]) + L": " + FormatCount(m_result.skipped[i]) + L"\r\n";
    issues += Localization::Lookup(IDS_CP_ESTIMATE_NOTE);
    SetText(IDC_CP_ISSUES, issues);
    const auto state = m_result.cancelled ? IDS_CP_CANCELLED : m_result.limitReached ? IDS_CP_LIMIT :
        m_result.incomplete ? IDS_CP_INCOMPLETE : m_result.candidates.empty() ? IDS_CP_EMPTY : IDS_CP_COMPLETE;
    SetText(IDC_CP_STATUS, Localization::Format(IDS_CP_FINISHED, Localization::Lookup(state),
        FormatCount(m_result.checkedFiles), FormatCount(m_result.failures), FormatCount((GetTickCount64() - m_started) / 1000)));
    SetText(IDC_CP_DETAILS, Localization::Lookup(IDS_CP_SELECT_HINT));
    m_internalChange = false;
    UpdateSummary();
}
void CCleanupPreviewDlg::UpdateSummary()
{
    std::uint64_t bytes = 0, selectedBytes = 0, selected = 0;
    for (size_t i = 0; i < m_result.candidates.size(); ++i)
    {
        bytes += m_result.candidates[i].allocatedBytes;
        if (m_selected[i]) { ++selected; selectedBytes += m_result.candidates[i].allocatedBytes; }
    }
    SetText(IDC_CP_TOTALS, Localization::Format(IDS_CP_TOTALS, FormatCount(m_result.candidates.size()), FormatBytes(bytes),
        FormatCount(selected), FormatBytes(selectedBytes)) + Localization::Format(IDS_CP_VISIBLE, FormatCount(m_visible.size())));
    GetDlgItem(IDC_CP_PLAN).EnableWindow(!m_scanning && m_valid && !m_result.incomplete && !m_result.cancelled &&
        !m_result.limitReached && m_result.failures == 0 && selected != 0);
}
void CCleanupPreviewDlg::OnPlan()
{
    if (m_scanning || !m_valid || m_result.incomplete || m_result.cancelled || m_result.limitReached ||
        m_result.failures != 0 || std::ranges::none_of(m_selected, [](bool value) { return value; })) return;
    CCleanupPlanDlg(this, m_result, m_selected, m_scanOptions).ShowModal();
}
void CCleanupPreviewDlg::OnWindowsRecommendations() { OpenWindowsCleanup(true); }
void CCleanupPreviewDlg::OnWindowsDiskCleanup() { OpenWindowsCleanup(false); }
void CCleanupPreviewDlg::OnNuGetHttp()
{
    OpenOfficialCache();
}
void CCleanupPreviewDlg::OnPipCache()
{
    SelectOfficialEnvironment(Cleanup::NuGetHttp::Provider::Pip);
}
void CCleanupPreviewDlg::OnNpmCache()
{
    SelectOfficialEnvironment(Cleanup::NuGetHttp::Provider::Npm);
}
void CCleanupPreviewDlg::SelectOfficialEnvironment(const Cleanup::NuGetHttp::Provider provider)
{
    if (m_scanning) return;
    const bool npm = provider == Cleanup::NuGetHttp::Provider::Npm;
    std::wstring tool;
    if (::GetEnvironmentVariableW(L"WDS_CLEANUP_TEST_ROOT", nullptr, 0) != 0)
    {
        // A fixture never falls back to interactive discovery or a different runtime environment.
        if (!m_fixture || !m_optionsValid) { SetText(IDC_CP_STATUS, Localization::Lookup(npm ? IDS_NP_UNAVAILABLE : IDS_PH_UNAVAILABLE)); return; }
        const auto variable = npm ? L"WDS_NPM_TEST_NODE" : L"WDS_PIP_TEST_PYTHON";
        const auto length = ::GetEnvironmentVariableW(variable, nullptr, 0);
        if (length != 0)
        {
            tool.resize(length);
            const auto copied = ::GetEnvironmentVariableW(variable, tool.data(), length);
            if (copied == 0 || copied >= length) tool.clear();
            else tool.resize(copied);
        }
        if (tool.empty()) { SetText(IDC_CP_STATUS, Localization::Lookup(npm ? IDS_NP_UNAVAILABLE : IDS_PH_UNAVAILABLE)); return; }
    }
    else
    {
        const auto selected = CDialog::PickFile(CDialog::FilePickerMode::Open, Localization::Lookup(npm ? IDS_NP_PICK_FILTER : IDS_PH_PICK_FILTER), this);
        if (!selected) { SetText(IDC_CP_STATUS, Localization::Lookup(npm ? IDS_NP_PICK_CANCELLED : IDS_PH_PICK_CANCELLED)); return; }
        tool = *selected;
    }
    OpenOfficialCache(std::move(tool), provider);
}
void CCleanupPreviewDlg::OpenOfficialCache(std::wstring tool, const Cleanup::NuGetHttp::Provider provider)
{
    if (m_scanning) return;
    CNuGetHttpCacheDlg dialog(this, [this]
    {
        const auto text = m_exclusions.GetText();
        // Failed saves and corrupt settings must not become an empty permissive policy.
        return Cleanup::NuGetHttp::ExclusionPolicy{ !m_exclusionsCorrupt && text == m_savedExclusionsText, text };
    }, std::move(tool), provider);
    dialog.ShowModal();
    if (dialog.Started())
    {
        InvalidatePreview();
        SetText(IDC_CP_STATUS, Localization::Lookup(provider == Cleanup::NuGetHttp::Provider::Npm ? IDS_NP_RESCAN :
            (provider == Cleanup::NuGetHttp::Provider::Pip ? IDS_PH_RESCAN : IDS_NH_RESCAN)));
    }
}
void CCleanupPreviewDlg::OpenWindowsCleanup(const bool recommendations)
{
    if (m_scanning) return;
    const auto title = Localization::Lookup(recommendations ? IDS_CP_WINDOWS_RECOMMENDATIONS : IDS_CP_WINDOWS_DISK);
    if (::MessageBoxW(Handle(), Localization::Format(IDS_CP_WINDOWS_CONFIRM, title).c_str(), title.c_str(),
        MB_OKCANCEL | MB_ICONINFORMATION | MB_DEFBUTTON2) != IDOK)
    {
        SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_WINDOWS_CANCELLED));
        return;
    }
    const auto& systemDirectory = GetSysDirectory();
    if (!recommendations && systemDirectory.empty())
    {
        SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_WINDOWS_UNAVAILABLE));
        SetText(IDC_CP_DETAILS, Localization::Lookup(IDS_CP_WINDOWS_MANUAL));
        return;
    }
    const std::wstring target = recommendations ? L"ms-settings:storagerecommendations" : systemDirectory + L"\\cleanmgr.exe";
    // Fixed official target, interactive only. No candidate paths, switches, runas or fallback.
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    launch.hwnd = Handle();
    launch.lpVerb = L"open";
    launch.lpFile = target.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&launch))
    {
        const auto error = GetLastError();
        SetText(IDC_CP_STATUS, error == ERROR_CANCELLED ? Localization::Lookup(IDS_CP_WINDOWS_CANCELLED) :
            Localization::Format(IDS_CP_WINDOWS_FAILED, title, error));
        SetText(IDC_CP_DETAILS, Localization::Lookup(IDS_CP_WINDOWS_MANUAL));
        return;
    }
    // Accepting a launch request proves neither cleanup completion nor released allocation.
    SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_WINDOWS_REQUESTED));
    SetText(IDC_CP_DETAILS, Localization::Lookup(IDS_CP_WINDOWS_MANUAL));
}
void CCleanupPreviewDlg::OnSelectAll()
{
    if (m_scanning || !m_valid) return;
    m_internalChange = true;
    for (const auto index : m_visible) m_selected[index] = true;
    m_files.Invalidate();
    m_internalChange = false;
    UpdateSummary();
}
void CCleanupPreviewDlg::OnSelectNone()
{
    if (m_scanning || !m_valid) return;
    m_internalChange = true;
    std::fill(m_selected.begin(), m_selected.end(), false);
    m_files.Invalidate();
    m_internalChange = false;
    UpdateSummary();
}
void CCleanupPreviewDlg::OnFileChanged(NMHDR* header, LRESULT* result)
{
    *result = 0;
    if (m_internalChange || m_scanning) return;
    const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
    const int row = m_files.GetFirstSelectedIndex();
    if (row >= 0 && static_cast<size_t>(row) < m_visible.size())
    {
        const auto& candidate = m_result.candidates[m_visible[row]];
        const auto rule = std::ranges::find(Cleanup::Rules(), candidate.ruleId, &Cleanup::Rule::id);
        SetText(IDC_CP_DETAILS, candidate.path + L"\r\n" +
            (rule == Cleanup::Rules().end() ? candidate.ruleId : RuleDetails(*rule)) +
            L"\r\n" + RiskName(candidate.risk, candidate.assessment));
    }
    if ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK)
    {
        if (change->iItem >= 0 && static_cast<size_t>(change->iItem) < m_visible.size())
            m_selected[m_visible[change->iItem]] = (change->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2);
        UpdateSummary();
    }
}
void CCleanupPreviewDlg::OnFileRangeChanged(NMHDR*, LRESULT* result)
{
    NMLISTVIEW change{};
    OnFileChanged(&change.hdr, result);
}
void CCleanupPreviewDlg::ToggleFile(const int row)
{
    if (m_scanning || !m_valid || row < 0 || static_cast<size_t>(row) >= m_visible.size()) return;
    const auto index = m_visible[row];
    m_selected[index] = !m_selected[index];
    m_files.RedrawItems(row, row);
    UpdateSummary();
}
void CCleanupPreviewDlg::OnFileClick(NMHDR* header, LRESULT* result)
{
    *result = 0;
    UINT flags = 0;
    const int row = m_files.HitTest(reinterpret_cast<NMLISTVIEW*>(header)->ptAction, &flags);
    if (flags & LVHT_ONITEMSTATEICON) ToggleFile(row);
}
void CCleanupPreviewDlg::OnFileKeyDown(NMHDR* header, LRESULT* result)
{
    *result = 0;
    if (reinterpret_cast<NMLVKEYDOWN*>(header)->wVKey == VK_SPACE)
        ToggleFile(m_files.GetNextItem(-1, LVNI_FOCUSED));
}
void CCleanupPreviewDlg::OnFileDisplayInfo(NMHDR* header, LRESULT* result)
{
    *result = 0;
    auto& item = reinterpret_cast<NMLVDISPINFO*>(header)->item;
    if (item.iItem < 0 || static_cast<size_t>(item.iItem) >= m_visible.size()) return;
    const auto index = m_visible[item.iItem];
    if (item.mask & LVIF_STATE)
        item.state = (item.state & ~LVIS_STATEIMAGEMASK) | INDEXTOSTATEIMAGEMASK(m_selected[index] ? 2 : 1);
    if (!(item.mask & LVIF_TEXT)) return;
    const auto& candidate = m_result.candidates[index];
    switch (item.iSubItem)
    {
    case 0: m_fileCell = candidate.path; break;
    case 1: m_fileCell = CategoryName(candidate.category); break;
    case 2: m_fileCell = FormatBytes(candidate.allocatedBytes); break;
    case 3: m_fileCell = candidate.ruleId + L" / v" + std::to_wstring(candidate.ruleVersion); break;
    case 4: m_fileCell = Localization::Lookup(candidate.reasonKey); break;
    case 5: m_fileCell = RiskName(candidate.risk, candidate.assessment); break;
    default: m_fileCell.clear(); break;
    }
    item.pszText = m_fileCell.data();
}
void CCleanupPreviewDlg::OnAddExclusion()
{
    if (const auto directory = CDialog::PickFolder(this); directory)
    {
        auto value = m_exclusions.GetText();
        if (!value.empty() && value.back() != L'\n') value += L"\r\n";
        m_exclusions.SetText(value + *directory);
        // Multiline WM_SETTEXT does not emit EN_CHANGE.
        OnConfigurationChanged();
    }
}
