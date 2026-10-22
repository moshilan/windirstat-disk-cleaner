// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CleanupPreview.h"

namespace Cleanup
{
    enum class Disposition : std::uint8_t { RecycleBin, PermanentDelete, WindowsCleanup };
    enum class PlanReason : std::uint8_t
    {
        None, IncompletePreview, InvalidSelection, ConfigurationChanged, RuleChanged,
        UnsafePath, Unavailable, IdentityChanged, FileChanged, NoLongerEligible,
        Duplicate, UnsupportedDisposition, Cancelled, VerificationFailed
    };
    enum class SimulationState : std::uint8_t { Simulated, Skipped, Cancelled, Failed };
    struct PlanItem
    {
        Candidate candidate;
        Disposition disposition = Disposition::RecycleBin;
        std::uint64_t changeTime = 0;
        PlanReason reason = PlanReason::None;
        std::uint32_t systemError = 0;
    };
    struct OperationRecord
    {
        size_t itemIndex = 0; // Resolves the immutable candidate, rule version and disposition in the plan.
        std::uint64_t startedAt = 0, finishedAt = 0;
        SimulationState state = SimulationState::Skipped;
        PlanReason reason = PlanReason::None;
        std::uint32_t systemError = 0;
        std::uint64_t estimatedReleaseUpperBound = 0;
    };
    struct SimulationResult
    {
        std::wstring planId;
        bool simulationOnly = true;
        std::uint64_t startedAt = 0, finishedAt = 0;
        std::vector<OperationRecord> records;
        std::uint64_t estimatedReleaseUpperBound = 0;
        std::uint64_t actualReleasedBytes = 0; // Always zero: there is no mutating backend.
    };
    class CleanupPlan
    {
    public:
        const std::wstring& Id() const { return m_id; }
        const std::vector<PlanItem>& Items() const { return m_items; }
        PlanReason Reason() const { return m_reason; }
        std::uint64_t EstimatedReleaseUpperBound() const { return m_estimated; }
    private:
        std::wstring m_id;
        ScanOptions m_options;
        std::vector<Rule> m_rules;
        std::vector<PlanItem> m_items;
        PlanReason m_reason = PlanReason::None;
        std::uint64_t m_estimated = 0;
        friend CleanupPlan BuildPlan(const ScanResult&, const std::vector<bool>&, const ScanOptions&, Disposition, std::stop_token);
        friend SimulationResult SimulatePlan(const CleanupPlan&, const ScanOptions&, std::stop_token);
    };
    // Copies only explicitly checked candidates. Incomplete previews cannot form a usable plan.
    CleanupPlan BuildPlan(const ScanResult& preview, const std::vector<bool>& selected,
        const ScanOptions& options, Disposition disposition = Disposition::RecycleBin, std::stop_token stop = {});
    // Fresh eligibility scan reuses the existing rules without changing them. No filesystem writes,
    // elevation, permission changes, application termination, automatic retry or fallback exist here.
    SimulationResult SimulatePlan(const CleanupPlan& plan, const ScanOptions& currentOptions, std::stop_token stop = {});
}
