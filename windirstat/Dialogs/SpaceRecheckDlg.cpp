// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "SpaceRecheckDlg.h"

namespace
{
    std::wstring Delta(const std::uint64_t before, const std::uint64_t after)
    {
        return std::wstring(after > before ? L"+" : after < before ? L"-" : L"") +
            FormatBytes(after >= before ? after - before : before - after);
    }
    std::wstring SampleTime(const std::uint64_t timestamp)
    {
        FILETIME file{ static_cast<DWORD>(timestamp), static_cast<DWORD>(timestamp >> 32) };
        SYSTEMTIME time{};
        if (!FileTimeToSystemTime(&file, &time)) return L"?";
        return std::format(L"{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC", time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond);
    }
}

CSpaceRecheckDlg::CSpaceRecheckDlg(WindowRef parent, const bool recommendations, std::wstring fixture)
    : MessageTarget(IDD_SPACE_RECHECK, nullptr, parent), m_fixture(std::move(fixture)), m_recommendations(recommendations) {}
CSpaceRecheckDlg::~CSpaceRecheckDlg()
{
    if (m_worker.joinable()) { m_worker.request_stop(); m_worker.join(); }
}
bool CSpaceRecheckDlg::OnInitDialog()
{
    CLayoutDialog::OnInitDialog();
    Localization::UpdateDialogs(*this);
    DarkMode::AdjustControls(*this);
    SetText(IDC_SR_TOOL, Localization::Lookup(m_recommendations ? IDS_CP_WINDOWS_RECOMMENDATIONS : IDS_CP_WINDOWS_DISK));
    wchar_t windows[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(windows, MAX_PATH);
    const auto root = !m_fixture.empty() ? m_fixture.substr(0, 3) :
        length >= 3 && length < MAX_PATH ? std::wstring(windows, 3) : L"";
    SetText(IDC_SR_DRIVE, root);
    SetText(IDC_SR_DIRECTORY, m_fixture);
    if (!m_fixture.empty()) SetText(IDC_SR_STATUS, Localization::Lookup(IDS_SR_FIXTURE));
    m_initializing = false;
    Controls();
    SetTimer(1, 100);
    return true;
}
void CSpaceRecheckDlg::Controls()
{
    for (const auto id : { IDC_SR_DRIVE, IDC_SR_DIRECTORY, IDC_SR_BROWSE, IDC_SR_BASELINE })
        GetDlgItem(id).EnableWindow(!m_busy);
    GetDlgItem(IDC_SR_RECHECK).EnableWindow(!m_busy && m_ready);
    GetDlgItem(IDC_SR_STOP).EnableWindow(m_busy);
}
void CSpaceRecheckDlg::OnTargetChanged()
{
    if (m_initializing || m_busy) return;
    m_ready = false;
    m_before = {};
    SetText(IDC_SR_RESULT, L"");
    SetText(IDC_SR_STATUS, Localization::Lookup(IDS_SR_TARGET_CHANGED));
    Controls();
}
void CSpaceRecheckDlg::OnBrowse()
{
    if (m_busy || !m_fixture.empty()) return;
    if (const auto directory = CDialog::PickFolder(this))
    {
        SetText(IDC_SR_DIRECTORY, *directory);
        OnTargetChanged();
    }
}
void CSpaceRecheckDlg::OnBaseline() { Start(true); }
void CSpaceRecheckDlg::OnRecheck() { if (m_ready) Start(false); }
void CSpaceRecheckDlg::Start(const bool baseline)
{
    if (m_busy) return;
    const auto root = GetDlgItem(IDC_SR_DRIVE).GetText();
    const auto directory = GetDlgItem(IDC_SR_DIRECTORY).GetText();
    // A validated preview fixture never falls through to a personal directory or real tool launch.
    if (!m_fixture.empty() && (root != m_fixture.substr(0, 3) || directory != m_fixture))
    {
        SetText(IDC_SR_STATUS, Localization::Lookup(IDS_SR_FIXTURE));
        return;
    }
    if (baseline) { m_ready = false; m_before = {}; SetText(IDC_SR_RESULT, L""); }
    m_targetRoot = root;
    m_targetDirectory = directory;
    m_baseline = baseline;
    m_finished = false;
    m_busy = true;
    Controls();
    SetText(IDC_SR_STATUS, Localization::Lookup(baseline ? IDS_SR_RECORDING : IDS_SR_CHECKING));
    m_worker = std::jthread([this, root, directory](std::stop_token stop)
    {
        try { m_sample = Cleanup::SpaceRecheck::Capture(root, directory, stop); }
        catch (...) { m_sample = {}; m_sample.error = ERROR_UNHANDLED_EXCEPTION; }
        m_finished.store(true);
    });
}
void CSpaceRecheckDlg::OnStop()
{
    if (!m_busy) return;
    m_worker.request_stop();
    GetDlgItem(IDC_SR_STOP).EnableWindow(false);
    SetText(IDC_SR_STATUS, Localization::Lookup(IDS_CP_STOPPING));
}
void CSpaceRecheckDlg::OnCancel()
{
    if (m_busy) { m_closePending = true; OnStop(); return; }
    ::KillTimer(Handle(), 1);
    CLayoutDialog::OnCancel();
}
void CSpaceRecheckDlg::OnTimer(UINT_PTR)
{
    if (!m_busy || !m_finished.load()) return;
    const bool cancelled = m_worker.get_stop_token().stop_requested();
    m_worker.join();
    m_busy = false;
    if (m_closePending) { OnCancel(); return; }
    using enum Cleanup::SpaceRecheck::State;
    if (GetDlgItem(IDC_SR_DRIVE).GetText() != m_targetRoot || GetDlgItem(IDC_SR_DIRECTORY).GetText() != m_targetDirectory)
    {
        OnTargetChanged();
        return;
    }
    if (cancelled || m_sample.state == Cancelled)
        SetText(IDC_SR_STATUS, Localization::Lookup(m_baseline ? IDS_SR_BASELINE_CANCELLED : IDS_SR_CANCELLED));
    else if (m_sample.state == Changed)
    {
        m_ready = false;
        SetText(IDC_SR_STATUS, Localization::Lookup(IDS_SR_CHANGED));
    }
    else if (m_sample.state != Ready)
        SetText(IDC_SR_STATUS, Localization::Format(IDS_SR_FAILED, m_sample.error));
    else if (m_baseline)
    {
        m_before = m_sample;
        m_ready = true;
        SetText(IDC_SR_RESULT, Localization::Lookup(IDS_SR_BEFORE) + L"\r\n" + Describe(m_before));
        Launch();
    }
    else if (!Cleanup::SpaceRecheck::Comparable(m_before, m_sample))
    {
        m_ready = false;
        SetText(IDC_SR_STATUS, Localization::Lookup(IDS_SR_CHANGED));
    }
    else ShowResult();
    Controls();
}
std::wstring CSpaceRecheckDlg::Describe(const Cleanup::SpaceRecheck::Snapshot& value) const
{
    auto text = Localization::Format(IDS_SR_SAMPLE, value.root, value.volumeId,
        SampleTime(value.started), SampleTime(value.finished), FormatBytes(value.capacity), FormatBytes(value.available));
    if (!value.directory.empty())
        text += L"\r\n" + Localization::Format(IDS_SR_DIRECTORY_SAMPLE, value.directory,
            FormatBytes(value.logical), FormatBytes(value.allocated), FormatCount(value.files), FormatCount(value.skipped)) +
            L"\r\n" + Localization::Lookup(value.directoryComplete ? IDS_SR_DIRECTORY_NOTE : IDS_SR_PARTIAL);
    return text;
}
void CSpaceRecheckDlg::ShowResult()
{
    auto text = Localization::Format(IDS_SR_DELTA, Delta(m_before.available, m_sample.available));
    if (!m_before.directory.empty())
    {
        if (m_before.directoryComplete && m_sample.directoryComplete && !m_before.directoryId.empty() &&
            m_before.directoryId == m_sample.directoryId)
            text += L"\r\n" + Localization::Format(IDS_SR_DIRECTORY_DELTA,
                Delta(m_before.logical, m_sample.logical), Delta(m_before.allocated, m_sample.allocated));
        else text += L"\r\n" + Localization::Lookup(IDS_SR_PARTIAL);
    }
    text += L"\r\n" + Localization::Lookup(IDS_SR_ATTRIBUTION) + L"\r\n\r\n" +
        Localization::Lookup(IDS_SR_BEFORE) + L"\r\n" + Describe(m_before) + L"\r\n\r\n" +
        Localization::Lookup(IDS_SR_AFTER) + L"\r\n" + Describe(m_sample);
    SetText(IDC_SR_RESULT, text);
    SetText(IDC_SR_STATUS, Localization::Lookup(IDS_SR_OBSERVED));
}
void CSpaceRecheckDlg::Launch()
{
    // The observation target is not passed as cleanup scope. Windows owns its own selection.
    if (!m_fixture.empty())
    {
        m_started = true;
        SetText(IDC_SR_STATUS, Localization::Lookup(IDS_SR_FIXTURE_READY));
        return;
    }
    const auto system = GetSysDirectory();
    if (!m_recommendations && system.empty())
    { SetText(IDC_SR_STATUS, Localization::Lookup(IDS_CP_WINDOWS_UNAVAILABLE)); return; }
    const auto target = m_recommendations ? std::wstring(L"ms-settings:storagerecommendations") : system + L"\\cleanmgr.exe";
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    launch.hwnd = Handle();
    launch.lpVerb = L"open";
    launch.lpFile = target.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch))
        SetText(IDC_SR_STATUS, Localization::Format(IDS_CP_WINDOWS_FAILED,
            Localization::Lookup(m_recommendations ? IDS_CP_WINDOWS_RECOMMENDATIONS : IDS_CP_WINDOWS_DISK), GetLastError()));
    else
    {
        m_started = true;
        SetText(IDC_SR_STATUS, Localization::Lookup(IDS_CP_WINDOWS_REQUESTED));
    }
}
