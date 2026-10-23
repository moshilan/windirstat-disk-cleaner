// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace Cleanup::NuGetHttp
{
    enum class State { Unavailable, Ready, Blocked, Success, Failed, Cancelled };
    enum class ExclusionFailure { None, Unavailable, Overlap, Changed };
    struct ExclusionPolicy
    {
        bool available = false; // Unavailable configuration must never mean an empty exclusion list.
        std::wstring text;
    };
    struct Measurement
    {
        std::optional<std::uint64_t> logicalBytes;
        std::optional<std::uint64_t> availableBytes;
    };
    struct Prepared;
    struct Preview
    {
        State state = State::Unavailable;
        std::wstring tool, cachePath, command, diagnostic;
        Measurement before;
        ExclusionFailure exclusionFailure = ExclusionFailure::None;
        std::wstring excludedDirectory;
        std::shared_ptr<const Prepared> prepared;
    };
    struct Result
    {
        State state = State::Failed;
        unsigned long exitCode = 0;
        bool started = false;
        Measurement before, after;
        std::wstring output, diagnostic;
        ExclusionFailure exclusionFailure = ExclusionFailure::None;
        std::wstring excludedDirectory;
    };
    // Discovery only. The caller must display the returned scope before asking for confirmation.
    Preview Prepare(const ExclusionPolicy& exclusions, std::stop_token stop = {});
    // Only an unmodified Ready preview from Prepare is executable; confirmation belongs to the caller.
    Result Execute(const Preview& preview, const ExclusionPolicy& currentExclusions, std::stop_token stop = {});
}
