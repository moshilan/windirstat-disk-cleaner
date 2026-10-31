// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "NuGetHttpCache.h"
#include <optional>

namespace Cleanup::BleachBit
{
    using ExclusionPolicy = NuGetHttp::ExclusionPolicy;
    enum class State { Unavailable, Ready, Blocked, Success, Failed, Cancelled };
    struct Measurement { std::optional<std::uint64_t> availableBytes; };
    struct Prepared;
    struct Preview
    {
        State state = State::Unavailable;
        std::wstring tool, version, userTemp, systemTemp, command, output, diagnostic;
        std::optional<std::uint64_t> files; // Engine Delete entries, including directories.
        std::shared_ptr<const Prepared> prepared;
    };
    struct Result
    {
        State state = State::Failed;
        bool started = false;
        unsigned long exitCode = 0;
        std::wstring output, diagnostic, postPreview;
        std::optional<std::uint64_t> files, errors, remainingFiles;
        // Observation of the volume containing userTemp, not attributed recovered space.
        Measurement before, after;
    };
    // Only built-in system.tmp is supported; its native keep list protects all Windows Temp.
    // No arbitrary options or arguments are accepted. Only standard current-user TEMP is cleaned.
    // Prepare runs the external engine's preview; the caller must show its whole scope.
    Preview Prepare(const ExclusionPolicy& exclusions, const std::wstring& absoluteTool = {}, std::stop_token stop = {});
    // Requires a Ready immutable preview and an explicit confirmation in the caller.
    // This is category execution: the engine enumerates again, independently of candidate checks.
    Result Execute(const Preview& preview, const ExclusionPolicy& exclusions, std::stop_token stop = {});
}
