// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pch.h"
#include "Layout.h"
#include "Cleanup/SpaceRecheck.h"

class CSpaceRecheckDlg final : public MessageTarget<CSpaceRecheckDlg, CLayoutDialog>
{
public:
    CSpaceRecheckDlg(WindowRef parent, bool recommendations, std::wstring fixture = {});
    ~CSpaceRecheckDlg() override;
    bool Started() const { return m_started; }
    static std::span<const RouteEntry> Routes();
protected:
    bool OnInitDialog() override;
    void OnOK() override {} // Enter cannot start an external tool.
    void OnCancel() override;
private:
    void OnBaseline();
    void OnRecheck();
    void OnTargetChanged();
    void OnBrowse();
    void OnStop();
    void OnTimer(UINT_PTR);
    void Start(bool baseline);
    void Controls();
    void ShowResult();
    void Launch();
    std::wstring Describe(const Cleanup::SpaceRecheck::Snapshot& value) const;
    Cleanup::SpaceRecheck::Snapshot m_before, m_sample;
    std::wstring m_fixture, m_targetRoot, m_targetDirectory;
    std::jthread m_worker;
    std::atomic_bool m_finished = false;
    bool m_recommendations, m_busy = false, m_baseline = false, m_ready = false;
    bool m_started = false, m_closePending = false, m_initializing = true;
};

inline std::span<const RouteEntry> CSpaceRecheckDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnBaseline>(BN_CLICKED, IDC_SR_BASELINE),
        Route::Control<&OnRecheck>(BN_CLICKED, IDC_SR_RECHECK),
        Route::Control<&OnTargetChanged>(EN_CHANGE, IDC_SR_DRIVE),
        Route::Control<&OnTargetChanged>(EN_CHANGE, IDC_SR_DIRECTORY),
        Route::Control<&OnBrowse>(BN_CLICKED, IDC_SR_BROWSE),
        Route::Control<&OnStop>(BN_CLICKED, IDC_SR_STOP),
        Route::Window<&OnTimer>(WM_TIMER),
        Route::Window<&OnCancel>(WM_CLOSE),
    };
    return entries;
}
