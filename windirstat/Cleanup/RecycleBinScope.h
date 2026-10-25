// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Cleanup::RecycleBinScope
{
    enum class State { Succeeded, Failed, Skipped, Cancelled, Blocked };
    enum class Reason
    {
        None, InvalidSelection, InvalidScope, MissingProbe, MissingEmpty, VolumeUnavailable,
        VolumeChanged, ProbeFailed, NotConfirmed, Cancelled, CancelCheckFailed,
        EmptyFailed, PreviousFailure, BatchBlocked
    };
    using Probe = std::function<std::optional<std::wstring>(const std::wstring& root)>;
    using Empty = std::function<std::int32_t(const std::wstring& root)>;
    using Cancelled = std::function<bool()>;
    struct Target { std::wstring root, volumeId; };
    class Scope
    {
    public:
        Scope() = default;
        bool Valid() const { return m_reason == Reason::None && !m_targets.empty(); }
        Reason GetReason() const { return m_reason; }
        const std::vector<Target>& GetTargets() const { return m_targets; }
        const std::wstring& GetFailureRoot() const { return m_failureRoot; }
    private:
        friend Scope Prepare(const std::vector<std::wstring>& roots, const Probe& probe);
        std::vector<Target> m_targets;
        Reason m_reason = Reason::InvalidScope;
        std::wstring m_failureRoot;
    };
    struct ItemResult
    {
        std::wstring root;
        State state = State::Skipped;
        Reason reason = Reason::BatchBlocked;
        std::int32_t error = 0;
        bool started = false;
    };
    struct Result
    {
        State state = State::Blocked;
        Reason reason = Reason::InvalidScope;
        std::vector<ItemResult> items;
        size_t started = 0;
        size_t Count(const State value) const
        {
            return static_cast<size_t>(std::count_if(items.begin(), items.end(),
                [value](const ItemResult& item) { return item.state == value; }));
        }
    };

    inline Scope Prepare(const std::vector<std::wstring>& roots, const Probe& probe)
    {
        Scope result;
        std::vector<std::wstring> normalized;
        std::array<bool, 26> seen{};
        if (roots.empty()) { result.m_reason = Reason::InvalidSelection; return result; }
        // Validate the entire explicit selection before making even a read-only probe.
        for (const auto& root : roots)
        {
            if (root.size() != 3 || root[1] != L':' || root[2] != L'\\' ||
                !((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')))
            { result.m_reason = Reason::InvalidSelection; result.m_failureRoot = root; return result; }
            const auto letter = root[0] >= L'a' && root[0] <= L'z' ? root[0] - L'a' + L'A' : root[0];
            const auto index = static_cast<size_t>(letter - L'A');
            if (!seen[index]) { normalized.push_back(std::wstring(1, static_cast<wchar_t>(letter)) + L":\\"); seen[index] = true; }
        }
        if (!probe) { result.m_reason = Reason::MissingProbe; return result; }
        for (const auto& root : normalized)
        {
            std::optional<std::wstring> id;
            try { id = probe(root); }
            catch (...) { result.m_reason = Reason::ProbeFailed; result.m_failureRoot = root; result.m_targets.clear(); return result; }
            if (!id || id->empty() || id->find_first_not_of(L" \t\r\n") == std::wstring::npos || id->find(L'\0') != std::wstring::npos)
            { result.m_reason = Reason::VolumeUnavailable; result.m_failureRoot = root; result.m_targets.clear(); return result; }
            result.m_targets.push_back({ root, *id });
        }
        result.m_reason = Reason::None;
        return result;
    }

    inline Result Execute(const Scope& scope, const bool confirmed, const Probe& probe, const Empty& empty,
        const Cancelled& cancelled = {})
    {
        Result result;
        for (const auto& target : scope.GetTargets()) result.items.push_back({ target.root });
        const auto cancelRemaining = [&](const size_t first, const Reason reason)
        {
            result.state = State::Cancelled; result.reason = reason;
            for (size_t i = first; i < result.items.size(); ++i)
            { result.items[i].state = State::Cancelled; result.items[i].reason = reason; }
        };
        const auto block = [&](const size_t failed, const Reason reason)
        {
            result.state = State::Blocked; result.reason = reason;
            if (failed < result.items.size()) { result.items[failed].state = State::Blocked; result.items[failed].reason = reason; }
        };
        if (!confirmed) { cancelRemaining(0, Reason::NotConfirmed); return result; }
        if (!scope.Valid()) { result.reason = scope.GetReason(); return result; }
        if (!probe || !empty) { block(0, !probe ? Reason::MissingProbe : Reason::MissingEmpty); return result; }
        const auto checkCancel = [&](const size_t next)
        {
            try
            {
                if (cancelled && cancelled()) { cancelRemaining(next, Reason::Cancelled); return true; }
            }
            catch (...) { block(next, Reason::CancelCheckFailed); return true; }
            return false;
        };
        const auto checkVolume = [&](const size_t index)
        {
            const auto& target = scope.GetTargets()[index];
            try
            {
                const auto id = probe(target.root);
                if (!id || id->empty() || id->find_first_not_of(L" \t\r\n") == std::wstring::npos || id->find(L'\0') != std::wstring::npos)
                { block(index, Reason::VolumeUnavailable); return false; }
                if (*id != target.volumeId) { block(index, Reason::VolumeChanged); return false; }
            }
            catch (...) { block(index, Reason::ProbeFailed); return false; }
            return true;
        };
        // A later unavailable/replaced volume blocks the whole batch before its first operation.
        for (size_t i = 0; i < scope.GetTargets().size(); ++i)
            if (checkCancel(0) || !checkVolume(i)) return result;
        for (size_t i = 0; i < scope.GetTargets().size(); ++i)
        {
            if (checkCancel(i) || !checkVolume(i) || checkCancel(i)) return result;
            auto& item = result.items[i];
            item.started = true; ++result.started;
            try { item.error = empty(item.root); }
            catch (...) { item.error = static_cast<std::int32_t>(0x80004005u); }
            if (item.error < 0)
            {
                item.state = State::Failed; item.reason = Reason::EmptyFailed;
                result.state = State::Failed; result.reason = Reason::EmptyFailed;
                for (size_t j = i + 1; j < result.items.size(); ++j) result.items[j].reason = Reason::PreviousFailure;
                return result;
            }
            item.state = State::Succeeded; item.reason = Reason::None;
        }
        result.state = State::Succeeded; result.reason = Reason::None;
        return result;
    }
}
