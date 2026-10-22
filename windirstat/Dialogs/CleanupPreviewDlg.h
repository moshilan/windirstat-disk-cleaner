// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pch.h"
#include "Layout.h"
#include "Cleanup/CleanupPreview.h"

class CCleanupPreviewDlg final : public MessageTarget<CCleanupPreviewDlg, CLayoutDialog>
{
public:
    explicit CCleanupPreviewDlg(WindowRef parent);
    ~CCleanupPreviewDlg() override;
    static std::span<const RouteEntry> Routes();
protected:
    bool OnInitDialog() override;
    void OnCancel() override;
    void OnOK() override {} // Enter never closes a preview or executes anything.
private:
    void OnScan();
    void OnStop();
    void OnSelectAll();
    void OnSelectNone();
    void OnAddExclusion();
    void OnConfigurationChanged();
    void OnCategoryChanged(NMHDR*, LRESULT*);
    void OnFileChanged(NMHDR*, LRESULT*);
    void OnTimer(UINT_PTR);
    void UpdateSummary();
    void SetScanning(bool scanning);
    void FillResult();
    void InvalidatePreview();
    bool ResolveOptions();
    static std::wstring CategoryName(Cleanup::Category category);
    RECT m_placement{};
    CListCtrl m_categories;
    CListCtrl m_files;
    CEdit m_exclusions;
    CProgressCtrl m_progressBar;
    Cleanup::ScanOptions m_options;
    Cleanup::ScanResult m_result;
    Cleanup::Progress m_progress;
    std::mutex m_mutex;
    std::jthread m_worker;
    std::atomic_bool m_finished = false;
    bool m_scanning = false;
    bool m_internalChange = false;
    bool m_closePending = false;
    bool m_fixture = false;
    bool m_valid = false;
    bool m_optionsValid = true;
    ULONGLONG m_started = 0;
};

inline std::span<const RouteEntry> CCleanupPreviewDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnScan>(BN_CLICKED, IDC_CP_SCAN),
        Route::Control<&OnStop>(BN_CLICKED, IDC_CP_CANCEL),
        Route::Control<&OnSelectAll>(BN_CLICKED, IDC_CP_SELECT_ALL),
        Route::Control<&OnSelectNone>(BN_CLICKED, IDC_CP_SELECT_NONE),
        Route::Control<&OnAddExclusion>(BN_CLICKED, IDC_CP_ADD_EXCLUSION),
        Route::Control<&OnConfigurationChanged>(EN_CHANGE, IDC_CP_EXCLUSIONS),
        Route::Notify<&OnCategoryChanged>(LVN_ITEMCHANGED, IDC_CP_CATEGORIES),
        Route::Notify<&OnFileChanged>(LVN_ITEMCHANGED, IDC_CP_FILES),
        Route::Window<&OnTimer>(WM_TIMER),
        Route::Window<&OnCancel>(WM_CLOSE),
    };
    return entries;
}
