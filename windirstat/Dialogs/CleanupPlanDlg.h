// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pch.h"
#include "Layout.h"
#include "Cleanup/CleanupPlan.h"

class CCleanupPlanDlg final : public MessageTarget<CCleanupPlanDlg, CLayoutDialog>
{
public:
    CCleanupPlanDlg(WindowRef parent, const Cleanup::ScanResult& preview,
        const std::vector<bool>& selected, const Cleanup::ScanOptions& options);
    ~CCleanupPlanDlg() override;
    static std::span<const RouteEntry> Routes();
protected:
    bool OnInitDialog() override;
    void OnOK() override {}
    void OnCancel() override;
private:
    void OnBuild();
    void OnSimulate();
    void OnStop();
    void OnTimer(UINT_PTR);
    void OnGetMinMaxInfo(MINMAXINFO*);
    void OnDisplayInfo(NMHDR*, LRESULT*);
    void OnSelection(NMHDR*, LRESULT*);
    void Start(bool simulate);
    void Refresh();
    void SetBusy(bool busy);
    CListCtrl m_files;
    Cleanup::ScanResult m_preview;
    std::vector<bool> m_selected;
    Cleanup::ScanOptions m_options;
    Cleanup::CleanupPlan m_plan;
    Cleanup::SimulationResult m_result;
    std::wstring m_cell;
    CSize m_minimumSize{ 0, 0 };
    std::jthread m_worker;
    std::atomic_bool m_finished = false;
    bool m_busy = false, m_error = false, m_simulated = false, m_closePending = false;
};

inline std::span<const RouteEntry> CCleanupPlanDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnBuild>(CBN_SELCHANGE, IDC_CPLAN_MODE),
        Route::Control<&OnSimulate>(BN_CLICKED, IDC_CPLAN_SIMULATE),
        Route::Control<&OnStop>(BN_CLICKED, IDC_CPLAN_STOP),
        Route::Notify<&OnDisplayInfo>(LVN_GETDISPINFO, IDC_CPLAN_FILES),
        Route::Notify<&OnSelection>(LVN_ITEMCHANGED, IDC_CPLAN_FILES),
        Route::Window<&OnTimer>(WM_TIMER),
        Route::Window<&OnGetMinMaxInfo>(WM_GETMINMAXINFO),
        Route::Window<&OnCancel>(WM_CLOSE),
    };
    return entries;
}
