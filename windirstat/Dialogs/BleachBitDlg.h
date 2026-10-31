// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pch.h"
#include "Layout.h"
#include "Cleanup/BleachBit.h"

class CBleachBitDlg final : public MessageTarget<CBleachBitDlg, CLayoutDialog>
{
public:
    CBleachBitDlg(WindowRef parent, std::function<Cleanup::BleachBit::ExclusionPolicy()> readExclusions);
    ~CBleachBitDlg() override;
    bool Started() const { return m_started; }
    static std::span<const RouteEntry> Routes();
protected:
    bool OnInitDialog() override;
    void OnOK() override {} // Enter cannot confirm or start cleaning.
    void OnCancel() override;
private:
    void OnClear();
    void OnStop();
    void OnTimer(UINT_PTR);
    void Start(bool execute);
    void Refresh();
    Cleanup::BleachBit::Preview m_preview;
    Cleanup::BleachBit::Result m_result;
    std::function<Cleanup::BleachBit::ExclusionPolicy()> m_readExclusions;
    std::jthread m_worker;
    std::atomic_bool m_finished = false;
    bool m_busy = false, m_executing = false, m_started = false, m_error = false, m_closePending = false;
};

inline std::span<const RouteEntry> CBleachBitDlg::Routes()
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
