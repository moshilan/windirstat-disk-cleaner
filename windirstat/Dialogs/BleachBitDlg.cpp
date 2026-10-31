// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "BleachBitDlg.h"

namespace
{
    std::wstring Number(const std::optional<std::uint64_t>& value)
    {
        return value ? std::to_wstring(*value) : Localization::Lookup(IDS_NH_UNKNOWN);
    }
    std::wstring AvailableChange(const Cleanup::BleachBit::Measurement& before, const Cleanup::BleachBit::Measurement& after)
    {
        if (!before.availableBytes || !after.availableBytes) return Localization::Lookup(IDS_NH_UNKNOWN);
        const auto a = *before.availableBytes, b = *after.availableBytes;
        return (b >= a ? L"+" : L"-") + std::to_wstring(b >= a ? b - a : a - b) + L" B";
    }
    std::wstring StateName(const Cleanup::BleachBit::State state)
    {
        using enum Cleanup::BleachBit::State;
        switch (state)
        {
        case Ready: return Localization::Lookup(IDS_BB_READY);
        case Success: return Localization::Lookup(IDS_BB_SUCCESS);
        case Failed: return Localization::Lookup(IDS_BB_FAILED);
        case Cancelled: return Localization::Lookup(IDS_BB_CANCELLED);
        case Blocked: return Localization::Lookup(IDS_BB_BLOCKED);
        default: return Localization::Lookup(IDS_BB_UNAVAILABLE);
        }
    }
}

CBleachBitDlg::CBleachBitDlg(WindowRef parent, std::function<Cleanup::BleachBit::ExclusionPolicy()> readExclusions)
    : MessageTarget(IDD_NUGET_HTTP, nullptr, parent), m_readExclusions(std::move(readExclusions)) {}
CBleachBitDlg::~CBleachBitDlg()
{
    if (m_worker.joinable()) { m_worker.request_stop(); m_worker.join(); }
}
bool CBleachBitDlg::OnInitDialog()
{
    CLayoutDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);
    ::SetWindowTextW(Handle(), Localization::Lookup(IDS_BB_TITLE).c_str());
    SetText(IDC_NH_NOTICE, Localization::Lookup(IDS_BB_NOTICE));
    SetText(IDC_NH_CLEAR, Localization::Lookup(IDS_BB_CLEAR));
    DarkMode::AdjustControls(*this);
    SetTimer(1, 100);
    Start(false);
    return true;
}
void CBleachBitDlg::Start(const bool execute)
{
    if (m_busy) return;
    const auto exclusions = m_readExclusions();
    m_busy = true;
    m_executing = execute;
    m_finished = false;
    m_error = false;
    GetDlgItem(IDC_NH_CLEAR).EnableWindow(false);
    GetDlgItem(IDC_NH_STOP).EnableWindow(true);
    SetText(IDC_NH_STATUS, Localization::Lookup(execute ? IDS_BB_RUNNING : IDS_BB_DETECTING));
    m_worker = std::jthread([this, execute, exclusions](std::stop_token stop)
    {
        try
        {
            if (execute) m_result = Cleanup::BleachBit::Execute(m_preview, exclusions, stop);
            else m_preview = Cleanup::BleachBit::Prepare(exclusions, {}, stop);
        }
        catch (...) { m_error = true; }
        m_finished.store(true);
    });
}
void CBleachBitDlg::OnClear()
{
    if (m_busy || m_executing || m_preview.state != Cleanup::BleachBit::State::Ready || !m_preview.prepared) return;
    if (::MessageBoxW(Handle(), Localization::Format(IDS_BB_CONFIRM, m_preview.userTemp, m_preview.systemTemp).c_str(),
        Localization::Lookup(IDS_BB_TITLE).c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
    {
        SetText(IDC_NH_STATUS, Localization::Lookup(IDS_NH_NOT_STARTED));
        return;
    }
    Start(true); // Read active exclusions again after confirmation; a dialog cannot override protection.
}
void CBleachBitDlg::OnStop()
{
    if (!m_busy) return;
    m_worker.request_stop();
    GetDlgItem(IDC_NH_STOP).EnableWindow(false);
    SetText(IDC_NH_STATUS, Localization::Lookup(IDS_NH_STOPPING));
}
void CBleachBitDlg::OnCancel()
{
    if (m_busy) { m_closePending = true; OnStop(); return; }
    ::KillTimer(Handle(), 1);
    CLayoutDialog::OnCancel();
}
void CBleachBitDlg::OnTimer(UINT_PTR)
{
    if (!m_busy || !m_finished.load()) return;
    if (m_worker.joinable()) m_worker.join();
    m_busy = false;
    m_started |= m_executing && m_result.started;
    GetDlgItem(IDC_NH_STOP).EnableWindow(false);
    Refresh();
    if (m_closePending && !m_started) OnCancel();
    m_closePending = false; // Keep started/partial cleanup results visible until explicitly closed again.
}
void CBleachBitDlg::Refresh()
{
    GetDlgItem(IDC_NH_CLEAR).EnableWindow(!m_error && !m_executing && m_preview.state == Cleanup::BleachBit::State::Ready);
    if (m_error)
    {
        SetText(IDC_NH_STATUS, Localization::Lookup(IDS_BB_FAILED));
        return;
    }
    const auto scope = Localization::Format(IDS_BB_SCOPE, m_preview.tool, m_preview.version,
        m_preview.userTemp, m_preview.systemTemp, m_preview.command, Number(m_preview.files));
    std::wstring details;
    if (m_executing)
    {
        details = Localization::Format(IDS_BB_RESULT, m_result.started ? 1 : 0, m_result.exitCode,
            Number(m_result.files), Number(m_result.errors), Number(m_result.remainingFiles),
            AvailableChange(m_result.before, m_result.after));
        details += L"\n" + m_result.diagnostic + L"\n\n" + m_result.output + L"\n\n" +
            Localization::Lookup(IDS_BB_POST_PREVIEW) + L"\n" + m_result.postPreview + L"\n\n" + scope;
        SetText(IDC_NH_STATUS, StateName(m_result.state));
    }
    else
    {
        details = scope + L"\n\n" + m_preview.diagnostic + L"\n\n" + m_preview.output;
        SetText(IDC_NH_STATUS, StateName(m_preview.state));
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
