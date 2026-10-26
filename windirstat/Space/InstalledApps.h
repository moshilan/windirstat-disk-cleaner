// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace Space::InstalledApps
{
    enum class Source { Machine32, Machine64, User32, User64, Msix };
    enum class State { Complete, Partial, Unavailable, Cancelled };
    struct Record
    {
        Source source = Source::Machine32;
        std::wstring key, name, publisher, version, installLocation;
        std::optional<std::uint32_t> estimatedKiB;
    };
    struct Item
    {
        Source source = Source::Machine32;
        std::wstring id, name, publisher, version, installLocation;
        std::optional<std::uint64_t> estimatedBytes;
    };
    struct SourceResult
    {
        Source source = Source::Machine32;
        State state = State::Unavailable;
        unsigned long error = 0;
        size_t skipped = 0;
    };
    struct LoadResult
    {
        std::vector<Item> items;
        std::array<SourceResult, 5> sources{};
        bool cancelled = false;
        bool invalidFixture = false;
    };
    // These operations only transform metadata; installLocation is never opened or scanned.
    std::optional<Item> Convert(const Record& record);
    void Deduplicate(std::vector<Item>& items);
    std::vector<size_t> Select(const std::vector<Item>& items, const std::wstring& query, bool descending);
    LoadResult Load(std::stop_token stop = {});
    // Only marked HKCU Software\WinDirStatTests\InstalledApps-<GUID> roots are accepted.
    // The five source-named subkeys contain synthetic registration fields. No real-source fallback.
    LoadResult LoadFixtureRegistry(const std::wstring& hkcuTestRoot, std::stop_token stop = {});
}
