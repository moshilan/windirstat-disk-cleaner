// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <functional>
#include <string>

namespace Space::InstallDirectory
{
    // An injected launch receives the same validated directory PIDL and explore request as ShellExecuteExW.
    // Returning success means the open request was accepted, not that Explorer has finished navigating.
    using Launch = std::function<BOOL(SHELLEXECUTEINFOW*)>;
    HRESULT Open(const std::wstring& registeredPath, HWND owner, const Launch& launch = {});
}
