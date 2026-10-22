// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "CleanupPlanDlg.h"

namespace
{
    std::wstring ReasonName(const Cleanup::PlanReason reason)
    {
        static constexpr std::array names{ IDS_CPLAN_PENDING, IDS_CPLAN_INCOMPLETE, IDS_CPLAN_SELECTION,
            IDS_CPLAN_CONFIG, IDS_CPLAN_RULE_CHANGED, IDS_CPLAN_UNSAFE, IDS_CPLAN_UNAVAILABLE,
            IDS_CPLAN_IDENTITY, IDS_CPLAN_CHANGED, IDS_CPLAN_INELIGIBLE, IDS_CPLAN_DUPLICATE,
            IDS_CPLAN_UNSUPPORTED, IDS_CPLAN_CANCELLED, IDS_CPLAN_VERIFY_FAILED };
        return Localization::Lookup(names[static_cast<size_t>(reason)]);
    }
    std::wstring DispositionName(const Cleanup::Disposition mode)
    {
        static constexpr std::array names{ IDS_CPLAN_RECYCLE, IDS_CPLAN_PERMANENT, IDS_CPLAN_WINDOWS };
        return Localization::Lookup(names[static_cast<size_t>(mode)]);
    }
    std::wstring StateName(const Cleanup::OperationRecord& record)
    {
        static constexpr std::array names{ IDS_CPLAN_SIMULATED, IDS_CPLAN_SKIPPED, IDS_CPLAN_CANCELLED, IDS_CPLAN_VERIFY_FAILED };
        return Localization::Lookup(names[static_cast<size_t>(record.state)]) +
            (record.reason == Cleanup::PlanReason::None ? L"" : L" | " + ReasonName(record.reason));
    }
}

CCleanupPlanDlg::CCleanupPlanDlg(WindowRef parent, const Cleanup::ScanResult& preview,
    const std::vector<bool>& selected, const Cleanup::ScanOptions& options)
    : MessageTarget(IDD_CLEANUP_PLAN, nullptr, parent), m_preview(preview), m_selected(selected), m_options(options) {}

CCleanupPlanDlg::~CCleanupPlanDlg()
{
    if (m_worker.joinable()) { m_worker.request_stop(); m_worker.join(); }
}

bool CCleanupPlanDlg::OnInitDialog()
{
    CLayoutDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);
    m_files.SubclassDlgItem(IDC_CPLAN_FILES, this);
    m_files.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const std::array columns{ IDS_CP_PATH, IDS_CP_RULE, IDS_CP_REASON, IDS_CP_RISK,
        IDS_CPLAN_DISPOSITION, IDS_CPLAN_UPPER, IDS_CPLAN_RESULT, IDS_CPLAN_ERROR };
    const std::array widths{ 320, 180, 340, 270, 220, 140, 320, 90 };
    for (int i = 0; i < static_cast<int>(columns.size()); ++i)
        m_files.InsertColumn(i, Localization::Lookup(columns[i]), LVCFMT_LEFT, widths[i]);
    const auto mode = GetDlgItem(IDC_CPLAN_MODE).Handle();
    for (const auto value : { Cleanup::Disposition::RecycleBin, Cleanup::Disposition::PermanentDelete, Cleanup::Disposition::WindowsCleanup })
    {
        const auto name = DispositionName(value);
        ::SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
    }
    ::SendMessageW(mode, CB_SETCURSEL, 0, 0);
    DarkMode::AdjustControls(*this);
    if (DarkMode::IsDarkModeActive())
    {
        m_files.SetBkColor(DarkMode::SystemColor(COLOR_WINDOW));
        m_files.SetTextBkColor(DarkMode::SystemColor(COLOR_WINDOW));
        m_files.SetTextColor(DarkMode::SystemColor(COLOR_WINDOWTEXT));
    }
    m_layout.AddControl(IDC_CPLAN_NOTICE, 0, 0, 1, 0);
    m_layout.AddControl(IDC_CPLAN_MODE, 0, 0, 1, 0);
    m_layout.AddControl(IDC_CPLAN_FILES, 0, 0, 1, 1);
    m_layout.AddControl(IDC_CPLAN_DETAILS, 0, 1, 1, 0);
    m_layout.AddControl(IDC_CPLAN_STATUS, 0, 1, 1, 0);
    m_layout.AddControl(IDC_CPLAN_SIMULATE, 0, 1, 0, 0);
    m_layout.AddControl(IDC_CPLAN_STOP, 0, 1, 0, 0);
    m_layout.AddControl(IDCANCEL, 1, 1, 0, 0);
    m_layout.OnInitDialog(true);
    const auto window = GetWindowRect();
    const auto client = GetClientRect();
    const auto files = GetChildWindowRect(m_files.Handle());
    const auto lastButton = GetChildWindowRect(GetDlgItem(IDC_CPLAN_STOP).Handle());
    const auto closeButton = GetChildWindowRect(GetDlgItem(IDCANCEL).Handle());
    m_minimumSize = CSize(lastButton.right + closeButton.Width() + files.left * 2 + window.Width() - client.Width(),
        window.Height() - files.Height() + ScaleForDpi(64));
    MONITORINFO monitor{ sizeof(monitor) };
    if (::GetMonitorInfoW(::MonitorFromWindow(Handle(), MONITOR_DEFAULTTONEAREST), &monitor))
    {
        const int width = std::min(window.Width(), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left - 16));
        const int height = std::min(window.Height(), static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top - 16));
        ::SetWindowPos(Handle(), nullptr, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2,
            width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetTimer(1, 100);
    OnBuild();
    return true;
}

void CCleanupPlanDlg::OnGetMinMaxInfo(MINMAXINFO* info)
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

void CCleanupPlanDlg::SetBusy(const bool busy)
{
    m_busy = busy;
    GetDlgItem(IDC_CPLAN_MODE).EnableWindow(!busy);
    GetDlgItem(IDC_CPLAN_SIMULATE).EnableWindow(!busy && !m_error &&
        m_plan.Reason() == Cleanup::PlanReason::None && !m_plan.Items().empty());
    GetDlgItem(IDC_CPLAN_STOP).EnableWindow(busy);
    m_files.EnableWindow(!busy);
}

void CCleanupPlanDlg::Start(const bool simulate)
{
    if (m_busy) return;
    const auto selectedMode = ::SendMessageW(GetDlgItem(IDC_CPLAN_MODE).Handle(), CB_GETCURSEL, 0, 0);
    if (selectedMode < 0 || selectedMode > 2) return;
    // Hide old rows before the worker replaces their immutable backing snapshot.
    m_files.SetItemCountEx(0);
    SetText(IDC_CPLAN_DETAILS, L"");
    m_finished = false;
    m_error = false;
    m_simulated = simulate;
    SetBusy(true);
    SetText(IDC_CPLAN_STATUS, Localization::Lookup(IDS_CPLAN_WORKING));
    m_worker = std::jthread([this, simulate, selectedMode](std::stop_token stop)
    {
        try
        {
            if (simulate) m_result = Cleanup::SimulatePlan(m_plan, m_options, stop);
            else
            {
                m_result = {};
                m_plan = Cleanup::BuildPlan(m_preview, m_selected, m_options, static_cast<Cleanup::Disposition>(selectedMode), stop);
            }
        }
        catch (...) { m_error = true; }
        m_finished.store(true);
    });
}
void CCleanupPlanDlg::OnBuild() { Start(false); }
void CCleanupPlanDlg::OnSimulate()
{
    if (!m_busy && !m_error && m_plan.Reason() == Cleanup::PlanReason::None && !m_plan.Items().empty()) Start(true);
}
void CCleanupPlanDlg::OnStop()
{
    if (!m_busy) return;
    m_worker.request_stop();
    GetDlgItem(IDC_CPLAN_STOP).EnableWindow(false);
    SetText(IDC_CPLAN_STATUS, Localization::Lookup(IDS_CPLAN_STOPPING));
}
void CCleanupPlanDlg::OnCancel()
{
    if (m_busy) { m_closePending = true; OnStop(); return; }
    ::KillTimer(Handle(), 1);
    CLayoutDialog::OnCancel();
}
void CCleanupPlanDlg::OnTimer(UINT_PTR)
{
    if (!m_busy || !m_finished.load()) return;
    if (m_worker.joinable()) m_worker.join();
    SetBusy(false);
    if (m_closePending) { OnCancel(); return; }
    Refresh();
}

void CCleanupPlanDlg::Refresh()
{
    if (m_error) { SetText(IDC_CPLAN_STATUS, Localization::Lookup(IDS_CPLAN_VERIFY_FAILED)); return; }
    m_files.SetItemCountEx(static_cast<int>(m_plan.Items().size()));
    if (m_plan.Reason() != Cleanup::PlanReason::None)
    { SetText(IDC_CPLAN_STATUS, ReasonName(m_plan.Reason())); return; }
    const auto upper = m_simulated ? m_result.estimatedReleaseUpperBound : m_plan.EstimatedReleaseUpperBound();
    size_t simulated = 0;
    for (const auto& record : m_result.records) if (record.state == Cleanup::SimulationState::Simulated) ++simulated;
    size_t skipped = m_result.records.size() - simulated;
    if (!m_simulated)
        for (const auto& item : m_plan.Items()) if (item.reason != Cleanup::PlanReason::None) ++skipped;
    SetText(IDC_CPLAN_STATUS, Localization::Format(IDS_CPLAN_TOTALS, m_plan.Id(), FormatCount(m_plan.Items().size()),
        FormatBytes(upper), FormatCount(simulated), FormatCount(skipped)));
}

void CCleanupPlanDlg::OnDisplayInfo(NMHDR* header, LRESULT* result)
{
    *result = 0;
    auto& display = reinterpret_cast<NMLVDISPINFO*>(header)->item;
    if (m_busy || !(display.mask & LVIF_TEXT) || display.iItem < 0 ||
        static_cast<size_t>(display.iItem) >= m_plan.Items().size()) return;
    const auto index = static_cast<size_t>(display.iItem);
    const auto& item = m_plan.Items()[index];
    const auto& candidate = item.candidate;
    const auto* record = index < m_result.records.size() ? &m_result.records[index] : nullptr;
    switch (display.iSubItem)
    {
    case 0: m_cell = candidate.path; break;
    case 1: m_cell = candidate.ruleId + L" / v" + std::to_wstring(candidate.ruleVersion); break;
    case 2: m_cell = Localization::Lookup(candidate.reasonKey); break;
    case 3: m_cell = Localization::Lookup(candidate.risk == Cleanup::Risk::Low ? IDS_CP_LOW :
        candidate.risk == Cleanup::Risk::High ? IDS_CP_HIGH : IDS_CP_MEDIUM) + L" | " +
        Localization::Lookup(candidate.assessment == Cleanup::Assessment::ManualReview ? IDS_CP_ASSESS_MANUAL : IDS_CP_ASSESS_REGENERABLE); break;
    case 4: m_cell = DispositionName(item.disposition); break;
    case 5: m_cell = FormatBytes(record ? record->estimatedReleaseUpperBound :
        item.reason == Cleanup::PlanReason::None && item.disposition == Cleanup::Disposition::PermanentDelete ? candidate.allocatedBytes : 0); break;
    case 6: m_cell = record ? StateName(*record) : ReasonName(item.reason); break;
    case 7: m_cell = std::to_wstring(record ? record->systemError : item.systemError); break;
    default: m_cell.clear(); break;
    }
    display.pszText = m_cell.data();
}
void CCleanupPlanDlg::OnSelection(NMHDR*, LRESULT* result)
{
    *result = 0;
    if (m_busy) return;
    const int row = m_files.GetFirstSelectedIndex();
    if (row < 0 || static_cast<size_t>(row) >= m_plan.Items().size()) return;
    const auto& item = m_plan.Items()[row];
    const auto& file = item.candidate;
    const auto status = static_cast<size_t>(row) < m_result.records.size() ?
        StateName(m_result.records[row]) : ReasonName(item.reason);
    SetText(IDC_CPLAN_DETAILS, file.path + L"\r\n" + file.ruleId + L" / v" + std::to_wstring(file.ruleVersion) +
        L"\r\n" + Localization::Lookup(file.reasonKey) + L"\r\n" + DispositionName(item.disposition) + L" | " + status +
        L"\r\n" + Localization::Format(IDS_CPLAN_METADATA, FormatBytes(file.logicalBytes), FormatBytes(file.allocatedBytes),
            std::to_wstring(file.volumeSerial), std::to_wstring(file.lastWriteTime), std::to_wstring(file.creationTime), std::to_wstring(item.changeTime)));
}
