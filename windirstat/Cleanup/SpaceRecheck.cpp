// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pch.h"
#include "SpaceRecheck.h"
#include "CleanupPreview.h"
#include "FinderBasic.h"
#include <limits>

namespace Cleanup::SpaceRecheck
{
    namespace
    {
        constexpr std::uint64_t EntryLimit = 100000;
        constexpr ULONGLONG TimeLimitMs = 30000;
        constexpr DWORD UnsafeAttributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
            FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_UNPINNED;
        struct Handle
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() { if (value != INVALID_HANDLE_VALUE) ::CloseHandle(value); }
        };
        std::uint64_t Now()
        {
            FILETIME time{};
            ::GetSystemTimeAsFileTime(&time);
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        }
        bool Equal(const std::wstring& a, const std::wstring& b)
        {
            return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
        }
        struct Volume
        {
            std::wstring guid, identity;
            std::uint64_t capacity = 0, available = 0;
        };
        bool ReadVolume(const std::wstring& root, Volume& result)
        {
            const auto type = ::GetDriveTypeW(root.c_str());
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            { ::SetLastError(ERROR_NOT_SUPPORTED); return false; }
            wchar_t guid[128]{}, mapping[32768]{}, guidMapping[32768]{};
            if (!::GetVolumeNameForVolumeMountPointW(root.c_str(), guid, static_cast<DWORD>(std::size(guid))) ||
                !::QueryDosDeviceW(root.substr(0, 2).c_str(), mapping, static_cast<DWORD>(std::size(mapping)))) return false;
            const std::wstring volume(guid);
            if (!volume.starts_with(L"\\\\?\\Volume{") || volume.back() != L'\\' ||
                !std::wstring_view(mapping).starts_with(L"\\Device\\"))
            { ::SetLastError(ERROR_NOT_SUPPORTED); return false; }
            // A drive-letter alias must resolve to the volume's native device, never SUBST.
            const auto device = volume.substr(4, volume.size() - 5);
            if (!::QueryDosDeviceW(device.c_str(), guidMapping, static_cast<DWORD>(std::size(guidMapping)))) return false;
            if (!Equal(mapping, guidMapping)) { ::SetLastError(ERROR_NOT_SUPPORTED); return false; }
            ULARGE_INTEGER capacity{}, available{};
            if (!::GetDiskFreeSpaceExW(root.c_str(), &available, &capacity, nullptr)) return false;
            if (available.QuadPart > capacity.QuadPart)
            { ::SetLastError(ERROR_INVALID_DATA); return false; }
            result = { volume, volume + L"|" + mapping, capacity.QuadPart, available.QuadPart };
            return true;
        }
        bool ReadDirectory(const std::wstring& path, const std::wstring& volume, std::wstring& identity)
        {
            if (!Cleanup::HasSafeAncestors(path)) { ::SetLastError(ERROR_CANT_ACCESS_FILE); return false; }
            Handle handle{ ::CreateFileW(Finder::MakeLongPathCompatible(path).c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
            if (handle.value == INVALID_HANDLE_VALUE) return false;
            BY_HANDLE_FILE_INFORMATION info{};
            if (!::GetFileInformationByHandle(handle.value, &info)) return false;
            if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & UnsafeAttributes))
            { ::SetLastError(ERROR_CANT_ACCESS_FILE); return false; }
            wchar_t finalPath[32768]{};
            const auto length = ::GetFinalPathNameByHandleW(handle.value, finalPath,
                static_cast<DWORD>(std::size(finalPath)), FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
            if (length == 0) return false;
            if (length >= std::size(finalPath) || length <= volume.size() ||
                !Equal(std::wstring(finalPath, volume.size()), volume))
            { ::SetLastError(ERROR_NOT_SAME_DEVICE); return false; }
            identity = std::to_wstring(info.dwVolumeSerialNumber) + L":" +
                std::to_wstring(info.nFileIndexHigh) + L":" + std::to_wstring(info.nFileIndexLow);
            return true;
        }
    }

    Snapshot Capture(const std::wstring& root, const std::wstring& directory, std::stop_token stop)
    {
        Snapshot result;
        result.started = Now();
        const auto finish = [&]() { result.finished = Now(); return result; };
        const auto cancelled = [&]()
        {
            if (!stop.stop_requested()) return false;
            result.state = State::Cancelled;
            result.directoryComplete = false;
            result.error = ERROR_CANCELLED;
            return true;
        };
        if (cancelled()) return finish();
        if (root.size() != 3 || root[1] != L':' || root[2] != L'\\' ||
            !((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')))
        { result.error = ERROR_INVALID_NAME; return finish(); }
        result.root = root;
        if (result.root[0] >= L'a' && result.root[0] <= L'z') result.root[0] -= L'a' - L'A';
        if (!directory.empty())
        {
            std::wstring error;
            if (!Cleanup::NormalizeDirectory(directory, result.directory, error) ||
                !Cleanup::IsWithin(result.directory, result.root))
            { result.error = ERROR_INVALID_NAME; return finish(); }
        }
        Volume before;
        if (!ReadVolume(result.root, before)) { result.error = ::GetLastError(); return finish(); }
        result.volumeId = before.identity;
        result.capacity = before.capacity;
        result.available = before.available;
        result.state = State::Ready;
        if (cancelled()) return finish();
        const auto incomplete = [&](const DWORD error)
        {
            result.directoryComplete = false;
            ++result.skipped;
            if (result.error == 0) result.error = error;
        };
        if (!result.directory.empty())
        {
            if (!ReadDirectory(result.directory, before.guid, result.directoryId)) incomplete(::GetLastError());
            else
            {
                std::vector<std::wstring> pending{ result.directory };
                const auto began = ::GetTickCount64();
                std::uint64_t entries = 0;
                while (!pending.empty())
                {
                    if (cancelled()) return finish();
                    if (entries >= EntryLimit || ::GetTickCount64() - began >= TimeLimitMs)
                    { incomplete(ERROR_TIMEOUT); break; }
                    auto path = std::move(pending.back());
                    pending.pop_back();
                    std::wstring id;
                    if (!ReadDirectory(path, before.guid, id)) { incomplete(::GetLastError()); continue; }
                    FinderBasic finder;
                    bool found = finder.FindFile(path);
                    while (found)
                    {
                        if (cancelled()) return finish();
                        if (++entries > EntryLimit || ::GetTickCount64() - began >= TimeLimitMs)
                        { incomplete(ERROR_TIMEOUT); break; }
                        const auto attributes = finder.GetAttributes();
                        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & UnsafeAttributes)) incomplete(ERROR_CANT_ACCESS_FILE);
                        else if (finder.IsDirectory()) pending.push_back(finder.GetFilePath());
                        else
                        {
                            const auto logical = finder.GetFileSizeLogical(), allocated = finder.GetFileSizePhysical();
                            if (logical > std::numeric_limits<std::uint64_t>::max() - result.logical ||
                                allocated > std::numeric_limits<std::uint64_t>::max() - result.allocated)
                            { incomplete(ERROR_ARITHMETIC_OVERFLOW); break; }
                            result.logical += logical;
                            result.allocated += allocated;
                            ++result.files;
                        }
                        found = finder.FindNext();
                    }
                    if (finder.GetResult().outcome != ScanOutcome::Complete) incomplete(ERROR_READ_FAULT);
                    std::wstring afterId;
                    if (!ReadDirectory(path, before.guid, afterId)) incomplete(::GetLastError());
                    else if (afterId != id)
                    { incomplete(ERROR_FILE_INVALID); result.state = State::Changed; break; }
                    // FinderBasic instances share one thread-local enumeration buffer.
                    // Process a child only after this directory's enumeration is finished.
                }
                std::wstring afterId;
                if (!ReadDirectory(result.directory, before.guid, afterId)) incomplete(::GetLastError());
                else if (afterId != result.directoryId)
                { incomplete(ERROR_FILE_INVALID); result.state = State::Changed; }
            }
        }
        if (cancelled()) return finish();
        Volume after;
        if (!ReadVolume(result.root, after))
        { result.state = State::Failed; result.error = ::GetLastError(); return finish(); }
        if (before.identity != after.identity || before.capacity != after.capacity)
        { result.state = State::Changed; result.error = ERROR_MEDIA_CHANGED; return finish(); }
        result.available = after.available;
        if (cancelled()) return finish();
        return finish();
    }
}
