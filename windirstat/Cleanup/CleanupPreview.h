// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace Cleanup
{
    enum class Category : std::uint8_t { Temporary, Chrome, Edge, Firefox, Pip, Count };
    inline constexpr size_t CategoryCount = static_cast<size_t>(Category::Count);
    enum class Risk : std::uint8_t { Low, Medium, High };
    enum class SkipReason : std::uint8_t { Recent, Pattern, Excluded, Unsafe, Hardlink, Streams, Metadata, Duplicate, Count };
    inline constexpr size_t SkipReasonCount = static_cast<size_t>(SkipReason::Count);

    // Definitions are deliberately limited to a known-folder anchor and audited relative paths.
    // No commands, plugins, database actions, or filesystem mutations are supported.
    struct Rule
    {
        std::wstring id;
        unsigned version = 1;
        Category category = Category::Temporary;
        std::wstring relativeRoot;
        unsigned minimumAgeDays = 7;
        Risk risk = Risk::Medium;
        std::wstring reasonKey;
    };
    struct Candidate
    {
        std::wstring path;
        std::wstring ruleId;
        unsigned ruleVersion = 1;
        Category category = Category::Temporary;
        Risk risk = Risk::Medium;
        std::wstring reasonKey;
        std::uint64_t logicalBytes = 0;
        std::uint64_t allocatedBytes = 0;
        std::uint64_t volumeSerial = 0;
        std::array<std::uint8_t, 16> fileId{};
        std::uint64_t lastWriteTime = 0;
        std::uint64_t creationTime = 0;
    };
    struct Summary { std::uint64_t files = 0; std::uint64_t allocatedBytes = 0; };
    struct Warning { std::wstring path; std::wstring message; bool resourceKey = false; };
    struct ScanOptions
    {
        std::wstring localAppData;
        std::vector<std::wstring> protectedDirectories;
        std::vector<std::wstring> excludedDirectories;
        std::array<bool, CategoryCount> enabled{ true, true, true, true, true };
        size_t maximumCandidates = 50000;
        // Zero uses the current system time. Tests may supply an explicit FILETIME timestamp.
        std::uint64_t now = 0;
    };
    struct Progress
    {
        Category category = Category::Temporary;
        std::uint64_t checkedFiles = 0;
        size_t candidates = 0;
        std::wstring currentPath;
    };
    struct ScanResult
    {
        std::vector<Candidate> candidates;
        std::array<Summary, CategoryCount> categories{};
        std::array<std::uint64_t, SkipReasonCount> skipped{};
        std::vector<Warning> warnings;
        std::uint64_t checkedFiles = 0;
        std::uint64_t failures = 0;
        bool cancelled = false;
        bool incomplete = false;
        bool limitReached = false;
    };
    const std::array<Rule, CategoryCount>& Rules();
    bool NormalizeDirectory(const std::wstring& input, std::wstring& normalized, std::wstring& error);
    bool IsWithin(const std::wstring& path, const std::wstring& directory);
    bool HasSafeAncestors(const std::wstring& path);
    // Resolve and validate the explicit GUI fixture override. Empty input means no override.
    bool ResolveTestFixture(const std::wstring& input, std::wstring& localAppData, std::wstring& error);
    ScanResult Scan(const ScanOptions& options, std::stop_token stop = {},
        const std::function<void(const Progress&)>& progress = {});
}
