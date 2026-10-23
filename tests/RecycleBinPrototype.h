// Isolated experiment only. Deliberately not linked into WinDirStat.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <windows.h>
#include <filesystem>
#include <memory>
#include "Cleanup/CleanupPlan.h"

namespace RecycleExperiment
{
    enum class State { Blocked, Recycled, Cancelled, Failed, Unknown };
    enum class Reason { BindingUnavailable, PlanRejected, NotOwned, Changed, UnsafePath, Access, Shell, Cancelled, None };
    enum class Control { Normal, CancelAtPre, ReplaceBeforePre, ReplaceAfterPre };
    struct Record
    {
        size_t itemIndex = 0;
        State state = State::Blocked;
        Reason reason = Reason::BindingUnavailable;
        Cleanup::PlanReason planReason = Cleanup::PlanReason::None;
        DWORD error = 0;
        HRESULT queued = E_PENDING, performed = E_PENDING, itemResult = E_PENDING, abortedQuery = E_PENDING;
        bool pre = false, post = false, aborted = false, recycleItem = false, recycledIdentityMatches = false;
        bool recycledReplacementMatches = false;
    };
    struct Review
    {
        std::wstring planId;
        std::vector<Record> records;
        size_t blocked = 0, cancelled = 0;
        // A successful Shell control never grants execution capability to a plan.
        bool readyForRealFiles = false;
        std::uint64_t actualReleasedBytes = 0;
    };
    class Prototype
    {
    public:
        Prototype();
        ~Prototype();
        Prototype(const Prototype&) = delete;
        Prototype& operator=(const Prototype&) = delete;
        size_t Create(); // CREATE_NEW, generated names, in-process ownership receipts only.
        Cleanup::CleanupPlan Plan() const;
        Review Evaluate(const Cleanup::CleanupPlan&, std::stop_token = {}) const;
        // UNSAFE FOR REAL FILES: controlled Shell semantics experiment, not the plan executor.
        // Only this instance's created receipts can reach the Shell. No arbitrary path argument.
        Record ShellControl(const Cleanup::CleanupPlan&, size_t itemIndex, Control = Control::Normal, std::stop_token = {});
        const std::filesystem::path& Path(size_t receipt) const;
        const std::filesystem::path& Root() const;
        bool IsOriginal(size_t receipt) const;
    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
