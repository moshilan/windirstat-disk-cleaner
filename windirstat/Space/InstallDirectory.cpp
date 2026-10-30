// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "InstallDirectory.h"
#include "Cleanup/CleanupPreview.h"
#include <shlobj.h>

namespace Space::InstallDirectory
{
    HRESULT Open(const std::wstring& registeredPath, HWND owner, const Launch& launch)
    {
        if (registeredPath.size() <= 3 || registeredPath[1] != L':' || registeredPath[2] != L'\\' ||
            !((registeredPath[0] >= L'A' && registeredPath[0] <= L'Z') || (registeredPath[0] >= L'a' && registeredPath[0] <= L'z')))
            return E_INVALIDARG;
        const auto type = ::GetDriveTypeW(registeredPath.substr(0, 3).c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        std::wstring path, error;
        if (!Cleanup::NormalizeDirectory(registeredPath, path, error) || !Cleanup::HasSafeAncestors(path))
            return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        struct DirectoryHandle
        {
            HANDLE value;
            ~DirectoryHandle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
        };
        // Verify list access without enumerating contents, and keep the leaf from being replaced during launch.
        DirectoryHandle directory{ ::CreateFileW((L"\\\\?\\" + path).c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
        if (directory.value == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(::GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(directory.value, &info)) return HRESULT_FROM_WIN32(::GetLastError());
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        struct ItemId
        {
            PIDLIST_ABSOLUTE value;
            ~ItemId() { ::CoTaskMemFree(value); }
        };
        ItemId target{ ::ILCreateFromPathW(path.c_str()) };
        if (!target.value) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        SHELLEXECUTEINFOW request{};
        request.cbSize = sizeof(request);
        request.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        request.hwnd = owner;
        request.lpVerb = L"explore";
        request.lpIDList = target.value;
        request.nShow = SW_SHOWNORMAL;
        // No command line, parent selection, alternate application, or fallback directory.
        ::SetLastError(ERROR_SUCCESS);
        if (launch ? launch(&request) : ::ShellExecuteExW(&request)) return S_OK;
        const auto failure = ::GetLastError();
        return HRESULT_FROM_WIN32(failure == ERROR_SUCCESS ? ERROR_GEN_FAILURE : failure);
    }
}
