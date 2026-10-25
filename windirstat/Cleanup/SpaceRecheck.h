// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <stop_token>
#include <string>

namespace Cleanup::SpaceRecheck
{
    enum class State { Ready, Failed, Cancelled, Changed };
    struct Snapshot
    {
        State state = State::Failed;
        std::wstring root, directory, volumeId, directoryId;
        std::uint64_t capacity = 0, available = 0, logical = 0, allocated = 0;
        std::uint64_t files = 0, skipped = 0, started = 0, finished = 0;
        bool directoryComplete = true;
        unsigned long error = 0;
    };
    // Read-only sampling. Directory totals count file paths, including hard-link aliases;
    // skipped entries and an exhausted time/entry budget make directory totals incomplete.
    Snapshot Capture(const std::wstring& root, const std::wstring& directory, std::stop_token stop = {});
    // Directory deltas additionally require directoryComplete and a nonempty directoryId
    // on both samples. A failed directory read does not discard a valid volume observation.
    inline bool Comparable(const Snapshot& before, const Snapshot& after)
    {
        return before.state == State::Ready && after.state == State::Ready &&
            !before.volumeId.empty() && before.root == after.root && before.directory == after.directory &&
            before.volumeId == after.volumeId &&
            (before.directoryId.empty() || after.directoryId.empty() || before.directoryId == after.directoryId) &&
            before.capacity == after.capacity;
    }
}
