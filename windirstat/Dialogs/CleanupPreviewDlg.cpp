// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "CleanupPreviewDlg.h"

namespace
{
    constexpr UINT_PTR CleanupTimer = 1;
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
        static const std::array suffixes{ L"\\**\\*.tmp; *.temp", L"\\Default|Profile <n>\\Cache\\Cache_Data\\**",
            L"\\Default|Profile <n>\\Cache\\Cache_Data\\**", L"\\<profile>\\cache2\\entries\\**", L"\\**" };
        return rule.relativeRoot + suffixes[static_cast<size_t>(rule.category)];
    }
}

CCleanupPreviewDlg::CCleanupPreviewDlg(WindowRef parent) : MessageTarget(IDD_CLEANUP_PREVIEW, nullptr, parent) {}
CCleanupPreviewDlg::~CCleanupPreviewDlg()
{
    if (m_worker.joinable()) { m_worker.request_stop(); m_worker.join(); }
}
std::wstring CCleanupPreviewDlg::CategoryName(const Cleanup::Category category)
{
    static constexpr std::array names{ IDS_CP_TEMP, IDS_CP_CHROME, IDS_CP_EDGE, IDS_CP_FIREFOX, IDS_CP_PIP };
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
        m_fixture = true;
        return true;
    }
    m_options.localAppData = KnownFolder(FOLDERID_LocalAppData);
    if (m_options.localAppData.empty()) return false;
    for (const auto* id : { &FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86,
        &FOLDERID_ProgramData, &FOLDERID_Desktop, &FOLDERID_Documents, &FOLDERID_Downloads,
        &FOLDERID_Pictures, &FOLDERID_Videos, &FOLDERID_Music, &FOLDERID_SkyDrive })
    {
        if (auto path = KnownFolder(*id); !path.empty()) m_options.protectedDirectories.push_back(std::move(path));
    }
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
        Cell(m_categories, static_cast<int>(i), 3, Localization::Lookup(IDS_CP_MEDIUM));
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
    if (m_fixture) SetText(IDC_CP_NOTICE, Localization::Lookup(IDS_CP_FIXTURE_NOTICE));
    SetText(IDC_CP_STATUS, Localization::Lookup(m_optionsValid ? IDS_CP_READY : IDS_CP_UNAVAILABLE));
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
    // Apply the two user offsets afterwards, keeping every pane usable.
    auto categories = GetChildWindowRect(m_categories.Handle());
    auto files = GetChildWindowRect(m_files.Handle());
    auto details = GetChildWindowRect(GetDlgItem(IDC_CP_DETAILS).Handle());
    const int minimum = ScaleForDpi(32);
    const int fileMinimum = ScaleForDpi(64);
    const int categoryMinimum = ScaleForDpi(65);
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
    for (const auto id : { IDC_CP_SORT_SIZE, IDC_CP_HIDE_ZERO })
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
    const CPen pen(PS_SOLID, 1, DarkMode::SystemColor(COLOR_3DSHADOW));
    const GdiObjectSelection selection(&dc, &pen);
    for (const int y : { (categories.bottom + toolbar.top) / 2, (files.bottom + details.top) / 2 })
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
    if (point.x < files.left || point.x >= files.right) return 0;
    if (point.y >= categories.bottom && point.y < toolbar.top) return 1;
    if (point.y >= files.bottom && point.y < details.top) return 2;
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
    else m_detailDelta -= delta;
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
    const int focused = m_files.GetFirstSelectedIndex();
    const size_t focus = focused >= 0 && static_cast<size_t>(focused) < m_visible.size() ?
        m_visible[focused] : m_result.candidates.size();
    m_internalChange = true;
    m_files.SetRedraw(false);
    ::SendMessageW(m_files.Handle(), LVM_DELETEALLITEMS, 0, 0);
    m_visible.clear();
    for (size_t i = 0; i < m_result.candidates.size(); ++i)
        if (!m_hideEmpty || m_result.candidates[i].logicalBytes != 0) m_visible.push_back(i);
    if (m_sortSize) std::stable_sort(m_visible.begin(), m_visible.end(), [this](size_t a, size_t b)
    {
        const auto left = m_result.candidates[a].allocatedBytes, right = m_result.candidates[b].allocatedBytes;
        return m_descending ? left > right : left < right;
    });
    for (size_t i = 0; i < m_visible.size(); ++i)
    {
        const auto index = m_visible[i];
        const auto& candidate = m_result.candidates[index];
        const int row = static_cast<int>(i);
        Row(m_files, row, candidate.path);
        Check(m_files, row, m_selected[index]);
        Cell(m_files, row, 1, CategoryName(candidate.category));
        Cell(m_files, row, 2, FormatBytes(candidate.allocatedBytes));
        Cell(m_files, row, 3, candidate.ruleId + L" / v" + std::to_wstring(candidate.ruleVersion));
        Cell(m_files, row, 4, Localization::Lookup(candidate.reasonKey));
        Cell(m_files, row, 5, Localization::Lookup(IDS_CP_MEDIUM));
        if (index == focus) m_files.SetItemState(row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (std::ranges::find(m_visible, focus) == m_visible.end()) SetText(IDC_CP_DETAILS, Localization::Lookup(IDS_CP_SELECT_HINT));
    m_files.SetRedraw(true);
    m_files.Invalidate();
    m_internalChange = false;
    SetText(IDC_CP_SORT_SIZE, Localization::Lookup(m_sortSize && m_descending ? IDS_CP_SORT_ASC : IDS_CP_SORT_DESC));
    UpdateSummary();
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
    for (const auto id : { IDC_CP_FILES, IDC_CP_SELECT_ALL, IDC_CP_SELECT_NONE, IDC_CP_SORT_SIZE, IDC_CP_HIDE_ZERO }) GetDlgItem(id).EnableWindow(!scanning && m_valid);
    m_progressBar.SetMarquee(scanning, 30);
}

void CCleanupPreviewDlg::InvalidatePreview()
{
    if (m_scanning || m_internalChange) return;
    m_internalChange = true;
    ::SendMessageW(m_files.Handle(), LVM_DELETEALLITEMS, 0, 0);
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
void CCleanupPreviewDlg::OnConfigurationChanged() { InvalidatePreview(); }
void CCleanupPreviewDlg::OnCategoryChanged(NMHDR* header, LRESULT* result)
{
    *result = 0;
    const auto* change = reinterpret_cast<NMLISTVIEW*>(header);
    if ((change->uChanged & LVIF_STATE) && ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK)) InvalidatePreview();
    else if (!m_scanning && !m_internalChange && change->iItem >= 0 && (change->uNewState & LVIS_SELECTED))
    {
        const auto index = static_cast<size_t>(change->iItem);
        const auto& rule = Cleanup::Rules()[index];
        SetText(IDC_CP_DETAILS, m_options.localAppData + L"\\" + RuleScope(rule) + L"\r\n" +
            Localization::Format(IDS_CP_RULE_DETAILS, rule.id, rule.version, RuleScope(rule), rule.minimumAgeDays) +
            L" | " + CoverageName(m_result.coverage[index].state));
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
    const auto exclusions = m_exclusions.GetText();
    for (size_t start = 0; start < exclusions.size();)
    {
        const size_t end = exclusions.find(L'\n', start);
        std::wstring line = exclusions.substr(start, end == std::wstring::npos ? end : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty())
        {
            std::wstring normalized, error;
            if (!Cleanup::NormalizeDirectory(line, normalized, error))
            {
                SetText(IDC_CP_STATUS, Localization::Lookup(IDS_CP_INVALID_EXCLUSION));
                SetText(IDC_CP_ISSUES, line + L"\r\n" + Localization::Lookup(IDS_CP_INVALID_EXCLUSION));
                return;
            }
            options.excludedDirectories.push_back(std::move(normalized));
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    InvalidatePreview();
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
}
void CCleanupPreviewDlg::OnSelectAll()
{
    if (m_scanning || !m_valid) return;
    m_internalChange = true;
    for (int i = 0; i < m_files.GetItemCount(); ++i) { m_selected[m_visible[i]] = true; Check(m_files, i, true); }
    m_internalChange = false;
    UpdateSummary();
}
void CCleanupPreviewDlg::OnSelectNone()
{
    if (m_scanning || !m_valid) return;
    m_internalChange = true;
    std::fill(m_selected.begin(), m_selected.end(), false);
    for (int i = 0; i < m_files.GetItemCount(); ++i) Check(m_files, i, false);
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
        const auto& rule = Cleanup::Rules()[static_cast<size_t>(candidate.category)];
        SetText(IDC_CP_DETAILS, candidate.path + L"\r\n" +
            Localization::Format(IDS_CP_RULE_DETAILS, candidate.ruleId, candidate.ruleVersion, RuleScope(rule), rule.minimumAgeDays) +
            L" | " + Localization::Lookup(candidate.reasonKey) + L" | " + Localization::Lookup(IDS_CP_MEDIUM));
    }
    if ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK)
    {
        if (change->iItem >= 0 && static_cast<size_t>(change->iItem) < m_visible.size())
            m_selected[m_visible[change->iItem]] = IsRowChecked(m_files, change->iItem);
        UpdateSummary();
    }
}
void CCleanupPreviewDlg::OnAddExclusion()
{
    if (const auto directory = CDialog::PickFolder(this); directory)
    {
        auto value = m_exclusions.GetText();
        if (!value.empty() && value.back() != L'\n') value += L"\r\n";
        m_exclusions.SetText(value + *directory);
        // Multiline WM_SETTEXT does not emit EN_CHANGE.
        InvalidatePreview();
    }
}
