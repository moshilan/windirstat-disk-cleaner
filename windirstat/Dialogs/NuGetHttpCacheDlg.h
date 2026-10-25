// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pch.h"
#include "Layout.h"
#include "Cleanup/NuGetHttpCache.h"

class CNuGetHttpCacheDlg final : public MessageTarget<CNuGetHttpCacheDlg, CLayoutDialog>
{
public:
    CNuGetHttpCacheDlg(WindowRef parent, std::function<Cleanup::NuGetHttp::ExclusionPolicy()> readExclusions,
        std::wstring tool = {}, Cleanup::NuGetHttp::Provider provider = Cleanup::NuGetHttp::Provider::NuGetHttp);
    ~CNuGetHttpCacheDlg() override;
    bool Started() const { return m_started; }
    static std::span<const RouteEntry> Routes();
protected:
    bool OnInitDialog() override;
    void OnOK() override {}
    void OnCancel() override;
private:
    void OnClear();
    void OnStop();
    void OnTimer(UINT_PTR);
    void Start(bool execute);
    void Refresh();
    Cleanup::NuGetHttp::Preview m_preview;
    Cleanup::NuGetHttp::Result m_result;
    std::function<Cleanup::NuGetHttp::ExclusionPolicy()> m_readExclusions;
    std::wstring m_tool;
    Cleanup::NuGetHttp::Provider m_provider;
    std::jthread m_worker;
    std::atomic_bool m_finished = false;
    bool m_busy = false, m_executing = false, m_started = false, m_error = false, m_closePending = false;
};

inline std::span<const RouteEntry> CNuGetHttpCacheDlg::Routes()
{
    static constexpr std::array entries
    {
        Route::Control<&OnClear>(BN_CLICKED, IDC_NH_CLEAR),
        Route::Control<&OnStop>(BN_CLICKED, IDC_NH_STOP),
        Route::Window<&OnTimer>(WM_TIMER),
        Route::Window<&OnCancel>(WM_CLOSE),
    };
    return entries;
}
