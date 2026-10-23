// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "NuGetHttpCacheDlg.h"

namespace
{
    std::wstring Bytes(const std::optional<std::uint64_t>& value)
    {
        return value ? std::to_wstring(*value) + L" B" : Localization::Lookup(IDS_NH_UNKNOWN);
    }
    std::wstring Change(const std::optional<std::uint64_t>& before, const std::optional<std::uint64_t>& after)
    {
        if (!before || !after) return Localization::Lookup(IDS_NH_UNKNOWN);
        return (*after >= *before ? L"+" : L"-") + std::to_wstring(*after >= *before ? *after - *before : *before - *after) + L" B";
    }
    std::wstring StateName(const Cleanup::NuGetHttp::State state)
    {
        using enum Cleanup::NuGetHttp::State;
        switch (state)
        {
        case Ready: return Localization::Lookup(IDS_NH_READY);
        case Success: return Localization::Lookup(IDS_NH_SUCCESS);
        case Failed: return Localization::Lookup(IDS_NH_FAILED);
        case Cancelled: return Localization::Lookup(IDS_NH_CANCELLED);
        case Blocked: return Localization::Lookup(IDS_NH_BLOCKED);
        default: return Localization::Lookup(IDS_NH_UNAVAILABLE);
        }
    }
    std::wstring ExclusionMessage(const Cleanup::NuGetHttp::ExclusionFailure failure, const std::wstring& directory)
    {
        using enum Cleanup::NuGetHttp::ExclusionFailure;
        switch (failure)
        {
        case Overlap: return Localization::Format(IDS_NH_EXCLUDED, directory);
        case Unavailable: return Localization::Lookup(IDS_NH_EXCLUSIONS_UNAVAILABLE);
        case Changed: return Localization::Lookup(IDS_NH_EXCLUSIONS_CHANGED);
        default: return {};
        }
    }
}

CNuGetHttpCacheDlg::CNuGetHttpCacheDlg(WindowRef parent, std::function<Cleanup::NuGetHttp::ExclusionPolicy()> readExclusions)
    : MessageTarget(IDD_NUGET_HTTP, nullptr, parent), m_readExclusions(std::move(readExclusions)) {}
CNuGetHttpCacheDlg::~CNuGetHttpCacheDlg()
{
    if (m_worker.joinable()) { m_worker.request_stop(); m_worker.join(); }
}
bool CNuGetHttpCacheDlg::OnInitDialog()
{
    CLayoutDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);
    DarkMode::AdjustControls(*this);
    SetTimer(1, 100);
    Start(false);
    return true;
}
void CNuGetHttpCacheDlg::Start(const bool execute)
{
    if (m_busy) return;
    // Read active rules on the UI thread, including again after the confirmation has closed.
    const auto exclusions = m_readExclusions();
    m_busy = true;
    m_executing = execute;
    m_finished = false;
    m_error = false;
    GetDlgItem(IDC_NH_CLEAR).EnableWindow(false);
    GetDlgItem(IDC_NH_STOP).EnableWindow(true);
    SetText(IDC_NH_STATUS, Localization::Lookup(execute ? IDS_NH_RUNNING : IDS_NH_DETECTING));
    m_worker = std::jthread([this, execute, exclusions](std::stop_token stop)
    {
        try
        {
            if (execute) m_result = Cleanup::NuGetHttp::Execute(m_preview, exclusions, stop);
            else m_preview = Cleanup::NuGetHttp::Prepare(exclusions, stop);
        }
        catch (...) { m_error = true; }
        m_finished.store(true);
    });
}
void CNuGetHttpCacheDlg::OnClear()
{
    if (m_busy || m_executing || m_preview.state != Cleanup::NuGetHttp::State::Ready || !m_preview.prepared) return;
    // The confirmation names the immutable official scope, never selected candidate rows.
    if (::MessageBoxW(Handle(), Localization::Format(IDS_NH_CONFIRM, m_preview.cachePath, m_preview.command).c_str(),
        Localization::Lookup(IDS_NH_TITLE).c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
    {
        SetText(IDC_NH_STATUS, Localization::Lookup(IDS_NH_NOT_STARTED));
        return;
    }
    Start(true);
}
void CNuGetHttpCacheDlg::OnStop()
{
    if (!m_busy) return;
    m_worker.request_stop();
    GetDlgItem(IDC_NH_STOP).EnableWindow(false);
    SetText(IDC_NH_STATUS, Localization::Lookup(IDS_NH_STOPPING));
}
void CNuGetHttpCacheDlg::OnCancel()
{
    if (m_busy) { m_closePending = true; OnStop(); return; }
    ::KillTimer(Handle(), 1);
    CLayoutDialog::OnCancel();
}
void CNuGetHttpCacheDlg::OnTimer(UINT_PTR)
{
    if (!m_busy || !m_finished.load()) return;
    if (m_worker.joinable()) m_worker.join();
    m_busy = false;
    m_started |= m_executing && m_result.started;
    GetDlgItem(IDC_NH_STOP).EnableWindow(false);
    Refresh();
    if (m_closePending && !m_started) OnCancel();
    m_closePending = false; // A started cleanup keeps its final/partial result visible until closed again.
}
void CNuGetHttpCacheDlg::Refresh()
{
    GetDlgItem(IDC_NH_CLEAR).EnableWindow(!m_error && !m_executing && m_preview.state == Cleanup::NuGetHttp::State::Ready);
    if (m_error)
    {
        SetText(IDC_NH_STATUS, Localization::Lookup(IDS_NH_FAILED));
        return;
    }
    std::wstring details = Localization::Format(IDS_NH_SCOPE, m_preview.tool, m_preview.cachePath, m_preview.command,
        Bytes(m_preview.before.logicalBytes));
    if (m_executing)
    {
        const auto scope = details;
        details = Localization::Format(IDS_NH_RESULT, m_result.started ? 1 : 0,
            m_result.exitCode, Bytes(m_result.before.logicalBytes), Bytes(m_result.after.logicalBytes),
            Change(m_result.before.logicalBytes, m_result.after.logicalBytes),
            Change(m_result.before.availableBytes, m_result.after.availableBytes));
        details += L"\r\n" + m_result.diagnostic + L"\r\n" + m_result.output + L"\r\n\r\n" + scope;
        const auto protection = ExclusionMessage(m_result.exclusionFailure, m_result.excludedDirectory);
        details = protection + (protection.empty() ? L"" : L"\r\n\r\n") + details;
        SetText(IDC_NH_STATUS, protection.empty() ? StateName(m_result.state) : protection);
    }
    else
    {
        details += L"\r\n\r\n" + m_preview.diagnostic;
        const auto protection = ExclusionMessage(m_preview.exclusionFailure, m_preview.excludedDirectory);
        details = protection + (protection.empty() ? L"" : L"\r\n\r\n") + details;
        SetText(IDC_NH_STATUS, protection.empty() ? StateName(m_preview.state) : protection);
    }
    std::wstring display;
    for (const auto c : details)
    {
        if (c == L'\r') continue;
        if (c == L'\n') display += L'\r';
        display += c;
    }
    SetText(IDC_NH_DETAILS, display);
}
