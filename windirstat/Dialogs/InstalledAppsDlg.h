// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pch.h"
#include "Layout.h"
#include "Space/InstalledApps.h"

class CInstalledAppsDlg final : public MessageTarget<CInstalledAppsDlg, CLayoutDialog>
{
public:
    explicit CInstalledAppsDlg(WindowRef parent);
    ~CInstalledAppsDlg() override;
    static std::span<const RouteEntry> Routes();
protected:
    bool OnInitDialog() override;
    void OnOK() override {} // Enter never opens a folder or a Windows removal page.
    void OnCancel() override;
private:
    void OnRefresh();
    void OnStop();
    void OnSearch();
    void OnSort();
    void OnColumn(NMHDR*, LRESULT*);
    void OnSelect(NMHDR*, LRESULT*);
    void OnDisplay(NMHDR*, LRESULT*);
    void OnLocate();
    void OnWindows();
    void OnTimer(UINT_PTR);
    void Fill();
    void Details();
    void Controls();
    const Space::InstalledApps::Item* Selected() const;
    static std::wstring SourceName(Space::InstalledApps::Source);
    CListCtrl m_list;
    Space::InstalledApps::LoadResult m_result;
    std::vector<size_t> m_visible;
    std::wstring m_fixtureKey, m_cell;
    std::jthread m_worker;
    std::atomic_bool m_finished = false;
    bool m_busy = false, m_closePending = false, m_error = false;
    bool m_fixture = false, m_descending = true, m_filling = false;
};

inline std::span<const RouteEntry> CInstalledAppsDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnRefresh>(BN_CLICKED, IDC_IA_REFRESH),
        Route::Control<&OnStop>(BN_CLICKED, IDC_IA_STOP),
        Route::Control<&OnSearch>(EN_CHANGE, IDC_IA_SEARCH),
        Route::Control<&OnSort>(BN_CLICKED, IDC_IA_SORT),
        Route::Control<&OnLocate>(BN_CLICKED, IDC_IA_LOCATE),
        Route::Control<&OnWindows>(BN_CLICKED, IDC_IA_WINDOWS),
        Route::Notify<&OnColumn>(LVN_COLUMNCLICK, IDC_IA_LIST),
        Route::Notify<&OnSelect>(LVN_ITEMCHANGED, IDC_IA_LIST),
        Route::Notify<&OnDisplay>(LVN_GETDISPINFO, IDC_IA_LIST),
        Route::Window<&OnTimer>(WM_TIMER),
        Route::Window<&OnCancel>(WM_CLOSE),
    };
    return entries;
}
