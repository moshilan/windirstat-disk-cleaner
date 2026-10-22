// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "CleanupPlan.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>

namespace Cleanup
{
    namespace
    {
        std::uint64_t Clock()
        {
            FILETIME time{};
            GetSystemTimeAsFileTime(&time);
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        }
        bool EqualPath(const std::wstring& a, const std::wstring& b)
        {
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), true) == CSTR_EQUAL;
        }
        struct PathLess
        {
            bool operator()(const std::wstring& a, const std::wstring& b) const
            {
                return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), true) == CSTR_LESS_THAN;
            }
        };
        bool SameConfiguration(const ScanOptions& a, const ScanOptions& b)
        {
            return a.localAppData == b.localAppData && a.roamingAppData == b.roamingAppData &&
                a.enabled == b.enabled && a.protectedDirectories == b.protectedDirectories &&
                a.excludedDirectories == b.excludedDirectories && a.maximumCandidates == b.maximumCandidates;
        }
        bool SameRule(const Rule& a, const Rule& b)
        {
            return a.id == b.id && a.version == b.version && a.category == b.category &&
                a.relativeRoot == b.relativeRoot && a.minimumAgeDays == b.minimumAgeDays && a.risk == b.risk &&
                a.reasonKey == b.reasonKey && a.anchor == b.anchor && a.layout == b.layout && a.assessment == b.assessment;
        }
        bool MatchesRule(const Candidate& candidate)
        {
            return std::ranges::any_of(Rules(), [&](const Rule& rule)
            {
                return candidate.ruleId == rule.id && candidate.ruleVersion == rule.version &&
                    candidate.category == rule.category && candidate.risk == rule.risk &&
                    candidate.reasonKey == rule.reasonKey && candidate.assessment == rule.assessment;
            });
        }
        bool SameIdentity(const Candidate& a, const Candidate& b)
        {
            return a.volumeSerial == b.volumeSerial && a.fileId == b.fileId;
        }
        bool SameMetadata(const Candidate& a, const Candidate& b)
        {
            return a.logicalBytes == b.logicalBytes && a.allocatedBytes == b.allocatedBytes &&
                a.lastWriteTime == b.lastWriteTime && a.creationTime == b.creationTime;
        }
        // A short lived metadata handle refuses concurrent writers/deleters. It grants no write
        // access and is never passed to any disposition API. This is not a future execution lock.
        PlanReason ReadSnapshot(const Candidate& expected, std::uint64_t& changeTime, std::uint32_t& error)
        {
            std::wstring path, diagnostic;
            error = 0;
            if (!NormalizeDirectory(expected.path, path, diagnostic) || !EqualPath(path, expected.path))
                return PlanReason::UnsafePath;
            if (!HasSafeAncestors(path))
            {
                const DWORD attributes = GetFileAttributesW((L"\\\\?\\" + path).c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) { error = GetLastError(); return PlanReason::Unavailable; }
                return PlanReason::UnsafePath;
            }
            const HANDLE file = CreateFileW((L"\\\\?\\" + path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file == INVALID_HANDLE_VALUE) { error = GetLastError(); return PlanReason::Unavailable; }
            FILE_STANDARD_INFO standard{};
            FILE_BASIC_INFO basic{};
            BY_HANDLE_FILE_INFORMATION identity{};
            const DWORD length = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            std::wstring final(length, L'\0');
            const DWORD copied = length == 0 ? 0 : GetFinalPathNameByHandleW(file, final.data(), length, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            const bool read = copied != 0 && copied < length &&
                GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) &&
                GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
                GetFileInformationByHandle(file, &identity);
            if (!read) error = GetLastError();
            CloseHandle(file);
            if (!read) return PlanReason::Unavailable;
            final.resize(copied);
            if (final.starts_with(L"\\\\?\\")) final.erase(0, 4);
            constexpr DWORD unsafe = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
                FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_UNPINNED |
                FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SYSTEM;
            if (!EqualPath(path, final) || !HasSafeAncestors(path) || (basic.FileAttributes & unsafe) != 0 ||
                standard.Directory || standard.DeletePending || standard.NumberOfLinks != 1 ||
                standard.EndOfFile.QuadPart < 0 || standard.AllocationSize.QuadPart < 0 ||
                basic.LastWriteTime.QuadPart < 0 || basic.CreationTime.QuadPart < 0 || basic.ChangeTime.QuadPart < 0)
                return PlanReason::UnsafePath;
            Candidate current;
            current.volumeSerial = identity.dwVolumeSerialNumber;
            const std::uint64_t index = (static_cast<std::uint64_t>(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow;
            std::memcpy(current.fileId.data(), &index, sizeof(index));
            if (!SameIdentity(expected, current)) return PlanReason::IdentityChanged;
            current.logicalBytes = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
            current.allocatedBytes = static_cast<std::uint64_t>(standard.AllocationSize.QuadPart);
            current.lastWriteTime = static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart);
            current.creationTime = static_cast<std::uint64_t>(basic.CreationTime.QuadPart);
            if (!SameMetadata(expected, current)) return PlanReason::FileChanged;
            changeTime = static_cast<std::uint64_t>(basic.ChangeTime.QuadPart);
            return PlanReason::None;
        }
    }

    CleanupPlan BuildPlan(const ScanResult& preview, const std::vector<bool>& selected,
        const ScanOptions& options, const Disposition disposition, const std::stop_token stop)
    {
        CleanupPlan plan;
        static std::atomic_uint64_t sequence = 0;
        plan.m_id = std::to_wstring(Clock()) + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
        plan.m_options = options;
        plan.m_rules.assign(Rules().begin(), Rules().end());
        if (preview.incomplete || preview.cancelled || preview.limitReached || preview.failures != 0)
        { plan.m_reason = PlanReason::IncompletePreview; return plan; }
        if (selected.size() != preview.candidates.size() || std::ranges::none_of(selected, [](bool value) { return value; }) ||
            (disposition != Disposition::RecycleBin && disposition != Disposition::PermanentDelete && disposition != Disposition::WindowsCleanup))
        { plan.m_reason = PlanReason::InvalidSelection; return plan; }
        std::set<std::pair<std::uint64_t, std::array<std::uint8_t, 16>>> identities;
        for (size_t i = 0; i < selected.size(); ++i)
        {
            if (!selected[i]) continue;
            PlanItem item{ preview.candidates[i], disposition };
            if (stop.stop_requested()) { plan.m_reason = PlanReason::Cancelled; item.reason = PlanReason::Cancelled; }
            else if (!MatchesRule(item.candidate)) item.reason = PlanReason::RuleChanged;
            else if (!identities.emplace(item.candidate.volumeSerial, item.candidate.fileId).second) item.reason = PlanReason::Duplicate;
            else if (disposition == Disposition::WindowsCleanup) item.reason = PlanReason::UnsupportedDisposition;
            else item.reason = ReadSnapshot(item.candidate, item.changeTime, item.systemError);
            if (item.reason == PlanReason::None && disposition == Disposition::PermanentDelete)
                plan.m_estimated += item.candidate.allocatedBytes;
            plan.m_items.push_back(std::move(item));
        }
        if (stop.stop_requested()) { plan.m_reason = PlanReason::Cancelled; plan.m_estimated = 0; }
        return plan;
    }

    SimulationResult SimulatePlan(const CleanupPlan& plan, const ScanOptions& currentOptions, const std::stop_token stop)
    {
        SimulationResult result;
        result.planId = plan.m_id;
        result.startedAt = Clock();
        PlanReason global = plan.m_reason;
        if (global == PlanReason::None && !SameConfiguration(plan.m_options, currentOptions)) global = PlanReason::ConfigurationChanged;
        if (global == PlanReason::None && (plan.m_rules.size() != Rules().size() ||
            !std::equal(plan.m_rules.begin(), plan.m_rules.end(), Rules().begin(), SameRule))) global = PlanReason::RuleChanged;
        ScanResult fresh;
        bool scanFailed = false;
        if (global == PlanReason::None && !stop.stop_requested() &&
            std::ranges::any_of(plan.m_items, [](const PlanItem& item) { return item.reason == PlanReason::None; }))
        {
            try { fresh = Scan(currentOptions, stop); }
            catch (...) { scanFailed = true; }
        }
        std::map<std::wstring, const Candidate*, PathLess> eligible;
        for (const auto& candidate : fresh.candidates) eligible.emplace(candidate.path, &candidate);
        for (size_t i = 0; i < plan.m_items.size(); ++i)
        {
            const auto& item = plan.m_items[i];
            OperationRecord record;
            record.itemIndex = i;
            record.startedAt = Clock();
            record.reason = global != PlanReason::None ? global : item.reason;
            record.systemError = item.systemError;
            if (stop.stop_requested())
            {
                record.state = SimulationState::Cancelled;
                record.reason = PlanReason::Cancelled;
            }
            else if (record.reason == PlanReason::None)
            {
                std::uint64_t changed = 0;
                record.reason = ReadSnapshot(item.candidate, changed, record.systemError);
                if (record.reason == PlanReason::None && changed != item.changeTime) record.reason = PlanReason::FileChanged;
                if (record.reason == PlanReason::None)
                {
                    const auto found = eligible.find(item.candidate.path);
                    if (scanFailed) record.reason = PlanReason::VerificationFailed;
                    else if (found == eligible.end()) record.reason = fresh.incomplete ? PlanReason::VerificationFailed : PlanReason::NoLongerEligible;
                    else if (found->second->ruleId != item.candidate.ruleId || found->second->ruleVersion != item.candidate.ruleVersion)
                        record.reason = PlanReason::RuleChanged;
                    else if (!SameIdentity(item.candidate, *found->second)) record.reason = PlanReason::IdentityChanged;
                    else if (!SameMetadata(item.candidate, *found->second)) record.reason = PlanReason::FileChanged;
                }
                if (record.reason == PlanReason::None)
                {
                    record.state = SimulationState::Simulated;
                    if (item.disposition == Disposition::PermanentDelete)
                        record.estimatedReleaseUpperBound = item.candidate.allocatedBytes;
                }
            }
            result.estimatedReleaseUpperBound += record.estimatedReleaseUpperBound;
            record.finishedAt = Clock();
            result.records.push_back(record);
        }
        result.finishedAt = Clock();
        return result;
    }
}
