// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pch.h"
#include "Layout.h"
#include "Cleanup/CleanupPreview.h"
#include "Cleanup/NuGetHttpCache.h"

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
    void OnPlan();
    void OnWindowsRecommendations();
    void OnWindowsDiskCleanup();
    void OnNuGetHttp();
    void OnPipCache();
    void OnNpmCache();
    void OnBleachBit();
    void SelectOfficialEnvironment(Cleanup::NuGetHttp::Provider provider);
    void OpenOfficialCache(std::wstring tool = {}, Cleanup::NuGetHttp::Provider provider = Cleanup::NuGetHttp::Provider::NuGetHttp);
    void OpenWindowsCleanup(bool recommendations);
    void OnStop();
    void OnSelectAll();
    void OnSelectNone();
    void OnAddExclusion();
    void OnConfigurationChanged();
    void OnCategoryChanged(NMHDR*, LRESULT*);
    void OnFileChanged(NMHDR*, LRESULT*);
    void OnFileDisplayInfo(NMHDR*, LRESULT*);
    void OnFileRangeChanged(NMHDR*, LRESULT*);
    void OnFileClick(NMHDR*, LRESULT*);
    void OnFileKeyDown(NMHDR*, LRESULT*);
    void ToggleFile(int row);
    void OnColumnClick(NMHDR*, LRESULT*);
    void OnSortSize();
    void OnFilter();
    void FillFiles();
    void OnSize(UINT, int, int);
    void OnLButtonDown(UINT, CPoint);
    void OnMouseMove(UINT, CPoint);
    void OnLButtonUp(UINT, CPoint);
    void OnCaptureChanged(WindowRef);
    bool OnSetCursor(WindowRef, UINT, UINT);
    int SplitterAt(CPoint) const;
    void LayoutPanes();
    void OnPaint();
    void OnTimer(UINT_PTR);
    void OnGetMinMaxInfo(MINMAXINFO*);
    void UpdateSummary();
    void SetScanning(bool scanning);
    void FillResult();
    void InvalidatePreview();
    bool ResolveOptions();
    void LoadExclusions();
    bool SaveExclusions();
    std::wstring RuleDetails(const Cleanup::Rule& rule) const;
    static std::wstring CategoryName(Cleanup::Category category);
    CListCtrl m_categories;
    CListCtrl m_files;
    CEdit m_exclusions;
    CProgressCtrl m_progressBar;
    Cleanup::ScanOptions m_options;
    Cleanup::ScanOptions m_scanOptions;
    Cleanup::ScanResult m_result;
    Cleanup::Progress m_progress;
    std::vector<size_t> m_visible;
    std::vector<bool> m_selected;
    bool m_sortSize = false;
    bool m_descending = true;
    bool m_layoutReady = false;
    bool m_hideEmpty = false;
    CSize m_minimumSize{ 0, 0 };
    int m_categoryDelta = 0;
    int m_detailDelta = 0;
    int m_issuesDelta = 0;
    int m_dragging = 0;
    int m_dragY = 0;
    std::mutex m_mutex;
    std::jthread m_worker;
    std::atomic_bool m_finished = false;
    bool m_scanning = false;
    bool m_internalChange = false;
    bool m_closePending = false;
    bool m_fixture = false;
    bool m_valid = false;
    bool m_optionsValid = true;
    bool m_exclusionsCorrupt = false;
    std::wstring m_savedExclusionsText;
    std::wstring m_fileCell;
    ULONGLONG m_started = 0;
};

inline std::span<const RouteEntry> CCleanupPreviewDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnScan>(BN_CLICKED, IDC_CP_SCAN),
        Route::Control<&OnPlan>(BN_CLICKED, IDC_CP_PLAN),
        Route::Control<&OnWindowsRecommendations>(BN_CLICKED, IDC_CP_WINDOWS_RECOMMENDATIONS),
        Route::Control<&OnWindowsDiskCleanup>(BN_CLICKED, IDC_CP_WINDOWS_DISK),
        Route::Control<&OnNuGetHttp>(BN_CLICKED, IDC_CP_NUGET),
        Route::Control<&OnPipCache>(BN_CLICKED, IDC_CP_PIP),
        Route::Control<&OnNpmCache>(BN_CLICKED, IDC_CP_NPM),
        Route::Control<&OnBleachBit>(BN_CLICKED, IDC_CP_BLEACHBIT),
        Route::Control<&OnStop>(BN_CLICKED, IDC_CP_CANCEL),
        Route::Control<&OnSelectAll>(BN_CLICKED, IDC_CP_SELECT_ALL),
        Route::Control<&OnSelectNone>(BN_CLICKED, IDC_CP_SELECT_NONE),
        Route::Control<&OnAddExclusion>(BN_CLICKED, IDC_CP_ADD_EXCLUSION),
        Route::Control<&OnConfigurationChanged>(EN_CHANGE, IDC_CP_EXCLUSIONS),
        Route::Notify<&OnCategoryChanged>(LVN_ITEMCHANGED, IDC_CP_CATEGORIES),
        Route::Notify<&OnFileChanged>(LVN_ITEMCHANGED, IDC_CP_FILES),
        Route::Notify<&OnFileDisplayInfo>(LVN_GETDISPINFO, IDC_CP_FILES),
        Route::Notify<&OnFileRangeChanged>(LVN_ODSTATECHANGED, IDC_CP_FILES),
        Route::Notify<&OnFileClick>(NM_CLICK, IDC_CP_FILES),
        Route::Notify<&OnFileClick>(NM_DBLCLK, IDC_CP_FILES),
        Route::Notify<&OnFileKeyDown>(LVN_KEYDOWN, IDC_CP_FILES),
        Route::Notify<&OnColumnClick>(LVN_COLUMNCLICK, IDC_CP_FILES),
        Route::Control<&OnSortSize>(BN_CLICKED, IDC_CP_SORT_SIZE),
        Route::Control<&OnFilter>(BN_CLICKED, IDC_CP_HIDE_ZERO),
        Route::Window<&OnSize>(WM_SIZE),
        Route::Window<&OnPaint>(WM_PAINT),
        Route::Window<&OnLButtonDown>(WM_LBUTTONDOWN),
        Route::Window<&OnMouseMove>(WM_MOUSEMOVE),
        Route::Window<&OnLButtonUp>(WM_LBUTTONUP),
        Route::Window<&OnCaptureChanged>(WM_CAPTURECHANGED),
        Route::Window<&OnSetCursor>(WM_SETCURSOR),
        Route::Window<&OnTimer>(WM_TIMER),
        Route::Window<&OnGetMinMaxInfo>(WM_GETMINMAXINFO),
        Route::Window<&OnCancel>(WM_CLOSE),
    };
    return entries;
}
