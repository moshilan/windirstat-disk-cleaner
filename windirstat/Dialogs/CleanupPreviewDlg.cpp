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
}

CCleanupPreviewDlg::CCleanupPreviewDlg(WindowRef parent) : MessageTarget(IDD_CLEANUP_PREVIEW, &m_placement, parent) {}
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
    m_categories.InsertColumn(0, Localization::Lookup(IDS_CP_CATEGORY), LVCFMT_LEFT, 270);
    m_categories.InsertColumn(1, Localization::Lookup(IDS_CP_COUNT), LVCFMT_RIGHT, 95);
    m_categories.InsertColumn(2, Localization::Lookup(IDS_CP_ALLOCATED), LVCFMT_RIGHT, 145);
    m_categories.InsertColumn(3, Localization::Lookup(IDS_CP_RISK), LVCFMT_LEFT, 150);
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
    m_layout.AddControl(IDCANCEL, 1, 1, 0, 0);
    m_layout.OnInitDialog(true);
    m_optionsValid = ResolveOptions();
    if (m_fixture) SetText(IDC_CP_NOTICE, Localization::Lookup(IDS_CP_FIXTURE_NOTICE));
    SetText(IDC_CP_STATUS, Localization::Lookup(m_optionsValid ? IDS_CP_READY : IDS_CP_UNAVAILABLE));
    SetText(IDC_CP_DETAILS, Localization::Lookup(IDS_CP_SELECT_HINT));
    SetScanning(false);
    UpdateSummary();
    SetTimer(CleanupTimer, 100);
    return true;
}

void CCleanupPreviewDlg::SetScanning(const bool scanning)
{
    m_scanning = scanning;
    for (const auto id : { IDC_CP_CATEGORIES, IDC_CP_EXCLUSIONS, IDC_CP_ADD_EXCLUSION }) GetDlgItem(id).EnableWindow(!scanning);
    GetDlgItem(IDC_CP_SCAN).EnableWindow(!scanning && m_optionsValid);
    GetDlgItem(IDC_CP_CANCEL).EnableWindow(scanning);
    for (const auto id : { IDC_CP_FILES, IDC_CP_SELECT_ALL, IDC_CP_SELECT_NONE }) GetDlgItem(id).EnableWindow(!scanning && m_valid);
    m_progressBar.SetMarquee(scanning, 30);
}

void CCleanupPreviewDlg::InvalidatePreview()
{
    if (m_scanning || m_internalChange) return;
    m_internalChange = true;
    ::SendMessageW(m_files.Handle(), LVM_DELETEALLITEMS, 0, 0);
    m_result = {};
    m_valid = false;
    for (size_t i = 0; i < Cleanup::CategoryCount; ++i)
    {
        Cell(m_categories, static_cast<int>(i), 1, L"0");
        Cell(m_categories, static_cast<int>(i), 2, FormatBytes(0));
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
    m_internalChange = true;
    m_files.SetRedraw(false);
    for (size_t i = 0; i < m_result.candidates.size(); ++i)
    {
        const auto& candidate = m_result.candidates[i];
        const int row = static_cast<int>(i);
        Row(m_files, row, candidate.path);
        Check(m_files, row, false);
        Cell(m_files, row, 1, CategoryName(candidate.category));
        Cell(m_files, row, 2, FormatBytes(candidate.allocatedBytes));
        Cell(m_files, row, 3, candidate.ruleId + L" / v" + std::to_wstring(candidate.ruleVersion));
        Cell(m_files, row, 4, Localization::Lookup(candidate.reasonKey));
        Cell(m_files, row, 5, Localization::Lookup(IDS_CP_MEDIUM));
    }
    m_files.SetRedraw(true);
    m_files.Invalidate();
    for (size_t i = 0; i < Cleanup::CategoryCount; ++i)
    {
        Cell(m_categories, static_cast<int>(i), 1, FormatCount(m_result.categories[i].files));
        Cell(m_categories, static_cast<int>(i), 2, FormatBytes(m_result.categories[i].allocatedBytes));
    }
    std::wstring issues = Localization::Lookup(IDS_CP_ESTIMATE_NOTE) + L"\r\n";
    const std::array reasons{ IDS_CP_SKIP_RECENT, IDS_CP_SKIP_PATTERN, IDS_CP_SKIP_EXCLUDED, IDS_CP_SKIP_UNSAFE,
        IDS_CP_SKIP_HARDLINK, IDS_CP_SKIP_STREAMS, IDS_CP_SKIP_METADATA, IDS_CP_SKIP_DUPLICATE };
    for (size_t i = 0; i < reasons.size(); ++i) if (m_result.skipped[i] != 0)
        issues += Localization::Lookup(reasons[i]) + L": " + FormatCount(m_result.skipped[i]) + L"\r\n";
    for (const auto& warning : m_result.warnings)
        issues += (warning.path.empty() ? L"" : warning.path + L": ") +
            (warning.resourceKey ? Localization::Lookup(warning.message) : warning.message) + L"\r\n";
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
        if (IsRowChecked(m_files, static_cast<int>(i))) { ++selected; selectedBytes += m_result.candidates[i].allocatedBytes; }
    }
    SetText(IDC_CP_TOTALS, Localization::Format(IDS_CP_TOTALS, FormatCount(m_result.candidates.size()), FormatBytes(bytes),
        FormatCount(selected), FormatBytes(selectedBytes)));
}
void CCleanupPreviewDlg::OnSelectAll()
{
    if (m_scanning || !m_valid) return;
    m_internalChange = true;
    for (int i = 0; i < m_files.GetItemCount(); ++i) Check(m_files, i, true);
    m_internalChange = false;
    UpdateSummary();
}
void CCleanupPreviewDlg::OnSelectNone()
{
    if (m_scanning || !m_valid) return;
    m_internalChange = true;
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
    if (row >= 0 && static_cast<size_t>(row) < m_result.candidates.size())
    {
        const auto& candidate = m_result.candidates[row];
        const auto& rule = Cleanup::Rules()[static_cast<size_t>(candidate.category)];
        SetText(IDC_CP_DETAILS, candidate.path + L"\r\n" +
            Localization::Format(IDS_CP_RULE_DETAILS, candidate.ruleId, candidate.ruleVersion, rule.relativeRoot, rule.minimumAgeDays) +
            L" | " + Localization::Lookup(candidate.reasonKey) + L" | " + Localization::Lookup(IDS_CP_MEDIUM));
    }
    if ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) UpdateSummary();
}
void CCleanupPreviewDlg::OnAddExclusion()
{
    if (const auto directory = CDialog::PickFolder(this); directory)
    {
        auto value = m_exclusions.GetText();
        if (!value.empty() && value.back() != L'\n') value += L"\r\n";
        m_exclusions.SetText(value + *directory);
    }
}
