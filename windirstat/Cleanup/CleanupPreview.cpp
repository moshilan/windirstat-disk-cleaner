// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
// Metadata-only candidate discovery. This unit has no application or GUI dependency.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CleanupPreview.h"
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace Cleanup
{
    namespace
    {
        constexpr DWORD UnsafeAttributes = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
            FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_UNPINNED;
        constexpr std::uint64_t Day = 24ULL * 60 * 60 * 10000000;
        struct Handle
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        };
        std::wstring Extended(const std::wstring& path) { return L"\\\\?\\" + path; }
        std::uint64_t Ticks(const FILETIME& value)
        {
            return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        }
        bool Equal(const std::wstring& a, const std::wstring& b)
        {
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), true) == CSTR_EQUAL;
        }
        bool ForbiddenName(const std::wstring& name)
        {
            static const std::array names{ L".git", L".svn", L".hg", L".ssh", L".aws", L".azure", L".gnupg", L"credentials" };
            return std::ranges::any_of(names, [&](const auto* item) { return Equal(name, item); });
        }
        bool Repository(const std::wstring& directory)
        {
            return GetFileAttributesW(Extended(directory + L"\\.git").c_str()) != INVALID_FILE_ATTRIBUTES ||
                GetFileAttributesW(Extended(directory + L"\\.hg").c_str()) != INVALID_FILE_ATTRIBUTES ||
                GetFileAttributesW(Extended(directory + L"\\.svn").c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        std::wstring SystemError(const DWORD code)
        {
            wchar_t* text = nullptr;
            const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
            std::wstring result = count != 0 ? std::wstring(text, count) : L"Windows error " + std::to_wstring(code);
            if (text != nullptr) LocalFree(text);
            while (!result.empty() && iswspace(result.back())) result.pop_back();
            return result;
        }
        bool FinalPath(const HANDLE file, std::wstring& path)
        {
            const DWORD size = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (size == 0) return false;
            std::wstring value(size, L'\0');
            const DWORD copied = GetFinalPathNameByHandleW(file, value.data(), size, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (copied == 0 || copied >= size) return false;
            value.resize(copied);
            if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
            std::wstring error;
            return NormalizeDirectory(value, path, error);
        }
        bool DefaultProfile(const std::wstring& name)
        {
            return Equal(name, L"Default") || (name.starts_with(L"Profile ") && name.size() > 8 &&
                std::all_of(name.begin() + 8, name.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; }));
        }
    }

    const std::array<Rule, CategoryCount>& Rules()
    {
        static const std::array<Rule, CategoryCount> rules{{
            { L"user-temp-v1", 1, Category::Temporary, L"Temp", 7, Risk::Medium, L"IDS_CP_REASON_TEMP" },
            { L"chrome-http-v1", 1, Category::Chrome, L"Google\\Chrome\\User Data", 7, Risk::Medium, L"IDS_CP_REASON_BROWSER" },
            { L"edge-http-v1", 1, Category::Edge, L"Microsoft\\Edge\\User Data", 7, Risk::Medium, L"IDS_CP_REASON_BROWSER" },
            { L"firefox-http-v1", 1, Category::Firefox, L"Mozilla\\Firefox\\Profiles", 7, Risk::Medium, L"IDS_CP_REASON_BROWSER" },
            { L"pip-http-v2", 1, Category::Pip, L"pip\\Cache\\http-v2", 7, Risk::Medium, L"IDS_CP_REASON_PIP" }
        }};
        return rules;
    }

    bool NormalizeDirectory(const std::wstring& input, std::wstring& normalized, std::wstring& error)
    {
        normalized.clear();
        std::wstring path = input;
        std::replace(path.begin(), path.end(), L'/', L'\\');
        if (path.size() < 4 || !iswalpha(path[0]) || path[1] != L':' || path[2] != L'\\' ||
            path.find_first_of(L"*?\r\n\"<>|") != std::wstring::npos || path.find(L':', 2) != std::wstring::npos)
        {
            error = L"Use an absolute local directory, not a drive root, wildcard, stream, or network path.";
            return false;
        }
        for (size_t start = 3; start < path.size();)
        {
            const size_t end = path.find(L'\\', start);
            const auto part = path.substr(start, end == std::wstring::npos ? end : end - start);
            if (part == L"." || part == L".." || (!part.empty() && (part.back() == L'.' || part.back() == L' ')))
            {
                error = L"Directory components cannot be dot aliases or end in a dot or space.";
                return false;
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
        if (size == 0) { error = SystemError(GetLastError()); return false; }
        std::wstring value(size, L'\0');
        const DWORD copied = GetFullPathNameW(path.c_str(), size, value.data(), nullptr);
        if (copied == 0 || copied >= size) { error = SystemError(GetLastError()); return false; }
        value.resize(copied);
        while (value.size() > 3 && value.back() == L'\\') value.pop_back();
        if (value.size() <= 3) { error = L"A whole drive is not a valid cleanup directory."; return false; }
        // Expand existing 8.3 aliases so directory exclusions share enumeration semantics.
        const auto extended = Extended(value);
        const DWORD longSize = GetLongPathNameW(extended.c_str(), nullptr, 0);
        if (longSize != 0)
        {
            std::wstring longPath(longSize, L'\0');
            const DWORD longCopied = GetLongPathNameW(extended.c_str(), longPath.data(), longSize);
            if (longCopied != 0 && longCopied < longSize)
            {
                longPath.resize(longCopied);
                if (longPath.starts_with(L"\\\\?\\")) longPath.erase(0, 4);
                value = std::move(longPath);
            }
        }
        normalized = std::move(value);
        return true;
    }

    bool IsWithin(const std::wstring& path, const std::wstring& directory)
    {
        return !directory.empty() && path.size() >= directory.size() &&
            CompareStringOrdinal(path.c_str(), static_cast<int>(directory.size()), directory.c_str(),
                static_cast<int>(directory.size()), true) == CSTR_EQUAL &&
            (path.size() == directory.size() || directory.back() == L'\\' || path[directory.size()] == L'\\');
    }

    bool HasSafeAncestors(const std::wstring& path)
    {
        std::wstring normalized, error;
        if (!NormalizeDirectory(path, normalized, error)) return false;
        for (size_t end = 3; ;)
        {
            const std::wstring prefix = normalized.substr(0, end);
            const DWORD attributes = GetFileAttributesW(Extended(prefix).c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & UnsafeAttributes) != 0) return false;
            if (end == normalized.size()) return true;
            end = normalized.find(L'\\', end + 1);
            if (end == std::wstring::npos) end = normalized.size();
        }
    }

    bool ResolveTestFixture(const std::wstring& input, std::wstring& localAppData, std::wstring& error)
    {
        std::wstring root;
        if (!NormalizeDirectory(input, root, error) || !HasSafeAncestors(root)) return false;
        wchar_t tempBuffer[32768]{};
        const DWORD copied = GetTempPathW(static_cast<DWORD>(std::size(tempBuffer)), tempBuffer);
        std::wstring temp;
        if (copied == 0 || copied >= std::size(tempBuffer) || !NormalizeDirectory(tempBuffer, temp, error) ||
            !IsWithin(root, temp) || root.size() <= temp.size() + 1)
        { error = L"Synthetic fixtures must be inside the system temporary test directory."; return false; }
        const auto tail = root.substr(temp.size() + 1);
        const auto slash = tail.find(L'\\');
        const auto owner = tail.substr(0, slash);
        if (!owner.starts_with(L"WinDirStatTests-") || slash == std::wstring::npos)
        { error = L"Synthetic fixtures must belong to a WinDirStatTests-* run."; return false; }
        const auto marker = root + L"\\.windirstat-cleanup-fixture";
        if (!HasSafeAncestors(marker)) { error = L"The synthetic fixture marker is missing or unsafe."; return false; }
        std::ifstream file{ std::filesystem::path(marker), std::ios::binary };
        std::string text;
        std::getline(file, text);
        if (!text.empty() && text.back() == '\r') text.pop_back();
        if (text != "WinDirStat read-only cleanup fixture v1")
        { error = L"The synthetic fixture marker is invalid."; return false; }
        localAppData = root + L"\\LocalAppData";
        if (!HasSafeAncestors(localAppData)) { error = L"Synthetic LocalAppData is missing or unsafe."; return false; }
        return true;
    }

    ScanResult Scan(const ScanOptions& options, const std::stop_token stop,
        const std::function<void(const Progress&)>& progress)
    {
        ScanResult result;
        std::wstring anchor, error;
        const auto warn = [&](const std::wstring& path, const std::wstring& reason, bool resourceKey = false)
        {
            result.incomplete = true;
            ++result.failures;
            if (result.warnings.size() < 100) result.warnings.push_back({ path, reason, resourceKey });
        };
        if (!NormalizeDirectory(options.localAppData, anchor, error) || !HasSafeAncestors(anchor))
        { warn(options.localAppData, L"IDS_CP_WARN_ANCHOR", true); return result; }
        // Legacy file IDs are reliable for this phase's local NTFS-only scope.
        wchar_t volume[32768]{}, filesystem[32]{};
        if (!GetVolumePathNameW(anchor.c_str(), volume, static_cast<DWORD>(std::size(volume))) ||
            !GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, filesystem, static_cast<DWORD>(std::size(filesystem))))
        { warn(anchor, SystemError(GetLastError())); return result; }
        if (GetDriveTypeW(volume) != DRIVE_FIXED || !Equal(filesystem, L"NTFS"))
        { warn(anchor, L"IDS_CP_WARN_FILESYSTEM", true); return result; }
        std::vector<std::wstring> excluded, protectedPaths;
        for (const auto& input : options.excludedDirectories)
        {
            std::wstring path;
            if (!NormalizeDirectory(input, path, error)) { warn(input, L"IDS_CP_INVALID_EXCLUSION", true); return result; }
            excluded.push_back(std::move(path));
        }
        for (const auto& input : options.protectedDirectories)
        {
            std::wstring path;
            if (!NormalizeDirectory(input, path, error)) { warn(input, L"IDS_CP_WARN_ANCHOR", true); return result; }
            protectedPaths.push_back(std::move(path));
        }
        const auto blocked = [&](const std::wstring& path)
        {
            return std::ranges::any_of(excluded, [&](const auto& item) { return IsWithin(path, item); }) ||
                std::ranges::any_of(protectedPaths, [&](const auto& item) { return IsWithin(path, item); });
        };
        if (blocked(anchor)) { ++result.skipped[static_cast<size_t>(SkipReason::Excluded)]; return result; }
        FILETIME current{};
        GetSystemTimeAsFileTime(&current);
        const auto now = options.now == 0 ? Ticks(current) : options.now;
        std::unordered_set<std::wstring> identities;
        const auto cancelled = [&]() { if (stop.stop_requested()) { result.cancelled = true; result.incomplete = true; } return result.cancelled; };
        const auto skipped = [&](SkipReason reason) { ++result.skipped[static_cast<size_t>(reason)]; };
        for (const auto& rule : Rules())
        {
            const auto index = static_cast<size_t>(rule.category);
            if (!options.enabled[index] || cancelled() || result.limitReached) continue;
            std::vector<std::wstring> roots;
            const std::wstring base = anchor + L"\\" + rule.relativeRoot;
            if (blocked(base)) { skipped(SkipReason::Excluded); continue; }
            const DWORD baseAttributes = GetFileAttributesW(Extended(base).c_str());
            if (baseAttributes == INVALID_FILE_ATTRIBUTES)
            {
                const DWORD code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) warn(base, SystemError(code));
                continue;
            }
            if (!HasSafeAncestors(base) || !(baseAttributes & FILE_ATTRIBUTE_DIRECTORY))
            { skipped(SkipReason::Unsafe); continue; }
            if (rule.category == Category::Chrome || rule.category == Category::Edge || rule.category == Category::Firefox)
            {
                WIN32_FIND_DATAW entry{};
                const HANDLE search = FindFirstFileExW(Extended(base + L"\\*").c_str(), FindExInfoBasic,
                    &entry, FindExSearchNameMatch, nullptr, 0);
                if (search == INVALID_HANDLE_VALUE) { warn(base, SystemError(GetLastError())); continue; }
                bool more = true;
                do
                {
                    const std::wstring name = entry.cFileName;
                    if (name != L"." && name != L".." && (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                        !(entry.dwFileAttributes & UnsafeAttributes) &&
                        (rule.category == Category::Firefox || DefaultProfile(name)))
                        roots.push_back(base + L"\\" + name + (rule.category == Category::Firefox ?
                            L"\\cache2\\entries" : L"\\Cache\\Cache_Data"));
                    if (cancelled()) break;
                    more = FindNextFileW(search, &entry) != false;
                } while (more);
                const DWORD code = GetLastError();
                FindClose(search);
                if (!more && code != ERROR_NO_MORE_FILES) warn(base, SystemError(code));
            }
            else roots.push_back(base);
            for (const auto& root : roots)
            {
                if (cancelled() || result.limitReached) break;
                const DWORD attributes = GetFileAttributesW(Extended(root).c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    const DWORD code = GetLastError();
                    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) warn(root, SystemError(code));
                    continue;
                }
                if (blocked(root)) { skipped(SkipReason::Excluded); continue; }
                if (!HasSafeAncestors(root) || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) { skipped(SkipReason::Unsafe); continue; }
                std::vector<std::pair<std::wstring, unsigned>> pending{ { root, 0 } };
                while (!pending.empty() && !cancelled() && !result.limitReached)
                {
                    auto [directory, depth] = std::move(pending.back()); pending.pop_back();
                    if (blocked(directory)) { skipped(SkipReason::Excluded); continue; }
                    if (!HasSafeAncestors(directory) || Repository(directory)) { skipped(SkipReason::Unsafe); continue; }
                    Handle dir{ CreateFileW(Extended(directory).c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
                    std::wstring finalDirectory;
                    if (dir.value == INVALID_HANDLE_VALUE || !FinalPath(dir.value, finalDirectory) ||
                        !IsWithin(finalDirectory, root) || blocked(finalDirectory))
                    { warn(directory, L"IDS_CP_WARN_VERIFY", true); continue; }
                    if (progress) progress({ rule.category, result.checkedFiles, result.candidates.size(), directory });
                    WIN32_FIND_DATAW entry{};
                    const HANDLE search = FindFirstFileExW(Extended(directory + L"\\*").c_str(), FindExInfoBasic,
                        &entry, FindExSearchNameMatch, nullptr, 0);
                    if (search == INVALID_HANDLE_VALUE)
                    {
                        const DWORD code = GetLastError();
                        if (code != ERROR_FILE_NOT_FOUND) warn(directory, SystemError(code));
                        continue;
                    }
                    bool more = true;
                    do
                    {
                        const std::wstring name = entry.cFileName;
                        const auto path = directory + L"\\" + name;
                        if (name != L"." && name != L"..")
                        {
                            if (entry.dwFileAttributes & UnsafeAttributes || ForbiddenName(name)) skipped(SkipReason::Unsafe);
                            else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                            {
                                if (depth < 128) pending.emplace_back(path, depth + 1);
                                else warn(path, L"IDS_CP_WARN_DEPTH", true);
                            }
                            else
                            {
                                ++result.checkedFiles;
                                if (blocked(path)) skipped(SkipReason::Excluded);
                                else if (entry.dwFileAttributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SYSTEM)) skipped(SkipReason::Unsafe);
                                else
                                {
                                    const size_t dot = name.rfind(L'.');
                                    const bool temporary = rule.category == Category::Temporary;
                                    if (temporary && (dot == std::wstring::npos || (!Equal(name.substr(dot), L".tmp") && !Equal(name.substr(dot), L".temp")))) skipped(SkipReason::Pattern);
                                    else if (Ticks(entry.ftLastWriteTime) > now || Ticks(entry.ftCreationTime) > now ||
                                        now - Ticks(entry.ftLastWriteTime) < rule.minimumAgeDays * Day ||
                                        now - Ticks(entry.ftCreationTime) < rule.minimumAgeDays * Day) skipped(SkipReason::Recent);
                                    else
                                    {
                                        Handle file{ CreateFileW(Extended(path).c_str(), FILE_READ_ATTRIBUTES,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr) };
                                        FILE_STANDARD_INFO standard{};
                                        FILE_BASIC_INFO basic{};
                                        BY_HANDLE_FILE_INFORMATION identity{};
                                        std::wstring final;
                                        if (file.value == INVALID_HANDLE_VALUE ||
                                            !GetFileInformationByHandleEx(file.value, FileStandardInfo, &standard, sizeof(standard)) ||
                                            !GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic, sizeof(basic)) ||
                                            !GetFileInformationByHandle(file.value, &identity) ||
                                            !FinalPath(file.value, final) || !IsWithin(final, root) || blocked(final) ||
                                            (basic.FileAttributes & (UnsafeAttributes | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SYSTEM)) ||
                                            standard.Directory || standard.DeletePending || standard.AllocationSize.QuadPart < 0 || standard.EndOfFile.QuadPart < 0)
                                            skipped(SkipReason::Metadata);
                                        else if (standard.NumberOfLinks != 1) skipped(SkipReason::Hardlink);
                                        else
                                        {
                                            WIN32_FIND_STREAM_DATA stream{};
                                            const HANDLE streams = FindFirstStreamW(Extended(path).c_str(), FindStreamInfoStandard, &stream, 0);
                                            bool safeStreams = streams != INVALID_HANDLE_VALUE;
                                            if (safeStreams)
                                            {
                                                do { if (std::wstring(stream.cStreamName) != L"::$DATA") safeStreams = false; }
                                                while (FindNextStreamW(streams, &stream));
                                                safeStreams = safeStreams && GetLastError() == ERROR_HANDLE_EOF;
                                                FindClose(streams);
                                            }
                                            if (!safeStreams) skipped(SkipReason::Streams);
                                            else
                                            {
                                                FILE_STANDARD_INFO after{};
                                                FILE_BASIC_INFO afterBasic{};
                                                if (!GetFileInformationByHandleEx(file.value, FileStandardInfo, &after, sizeof(after)) ||
                                                    !GetFileInformationByHandleEx(file.value, FileBasicInfo, &afterBasic, sizeof(afterBasic)) ||
                                                    after.EndOfFile.QuadPart != standard.EndOfFile.QuadPart || after.AllocationSize.QuadPart != standard.AllocationSize.QuadPart ||
                                                    afterBasic.LastWriteTime.QuadPart != basic.LastWriteTime.QuadPart ||
                                                    afterBasic.CreationTime.QuadPart != basic.CreationTime.QuadPart ||
                                                    afterBasic.FileAttributes != basic.FileAttributes || after.NumberOfLinks != 1 || after.DeletePending ||
                                                    basic.LastWriteTime.QuadPart < 0 || basic.CreationTime.QuadPart < 0 ||
                                                    static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart) > now ||
                                                    static_cast<std::uint64_t>(basic.CreationTime.QuadPart) > now ||
                                                    now - static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart) < rule.minimumAgeDays * Day ||
                                                    now - static_cast<std::uint64_t>(basic.CreationTime.QuadPart) < rule.minimumAgeDays * Day)
                                                    skipped(SkipReason::Metadata);
                                                else
                                                {
                                                    const std::uint64_t fileIndex = (static_cast<std::uint64_t>(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow;
                                                    const std::wstring key = std::to_wstring(identity.dwVolumeSerialNumber) + L":" + std::to_wstring(fileIndex);
                                                    if (!identities.insert(key).second) skipped(SkipReason::Duplicate);
                                                    else
                                                    {
                                                        Candidate candidate{ path, rule.id, rule.version, rule.category, rule.risk, rule.reasonKey,
                                                            static_cast<std::uint64_t>(standard.EndOfFile.QuadPart), static_cast<std::uint64_t>(standard.AllocationSize.QuadPart),
                                                            identity.dwVolumeSerialNumber, {}, static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart), static_cast<std::uint64_t>(basic.CreationTime.QuadPart) };
                                                        std::memcpy(candidate.fileId.data(), &fileIndex, sizeof(fileIndex));
                                                        result.categories[index].files++;
                                                        result.categories[index].allocatedBytes += candidate.allocatedBytes;
                                                        result.candidates.push_back(std::move(candidate));
                                                        if (result.candidates.size() >= options.maximumCandidates)
                                                        { result.limitReached = true; result.incomplete = true; }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                if (progress && (result.checkedFiles == 1 || result.checkedFiles % 64 == 0))
                                    progress({ rule.category, result.checkedFiles, result.candidates.size(), path });
                            }
                        }
                        if (cancelled() || result.limitReached) break;
                        more = FindNextFileW(search, &entry) != false;
                    } while (more);
                    const DWORD code = GetLastError();
                    FindClose(search);
                    if (!more && code != ERROR_NO_MORE_FILES) warn(directory, SystemError(code));
                }
            }
        }
        if (progress) progress({ Category::Temporary, result.checkedFiles, result.candidates.size(), L"" });
        return result;
    }
}
