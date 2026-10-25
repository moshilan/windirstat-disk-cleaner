// Pure in-memory scope tests: no drives, filesystem, GUI or Shell API are accessed.
#include "Cleanup/RecycleBinScope.h"
#include <iostream>
#include <map>
#include <stdexcept>
namespace RB = Cleanup::RecycleBinScope;
namespace
{
    unsigned checks = 0;
    void Check(const bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
    struct Fake
    {
        std::map<std::wstring, std::wstring> ids{ {L"C:\\", L"volume-C"}, {L"D:\\", L"volume-D"}, {L"E:\\", L"volume-E"} };
        std::vector<std::wstring> probes, emptied;
        RB::Probe Probe()
        {
            return [this](const std::wstring& root) -> std::optional<std::wstring>
            {
                probes.push_back(root);
                const auto it = ids.find(root);
                return it == ids.end() ? std::nullopt : std::optional(it->second);
            };
        }
        RB::Empty Empty()
        {
            return [this](const std::wstring& root) { emptied.push_back(root); return std::int32_t{ 0 }; };
        }
    };
    void Counts(const RB::Result& result)
    {
        Check(result.Count(RB::State::Succeeded) + result.Count(RB::State::Failed) + result.Count(RB::State::Skipped) +
            result.Count(RB::State::Cancelled) + result.Count(RB::State::Blocked) == result.items.size(), "Every target has exactly one final state");
        Check(result.started == static_cast<size_t>(std::count_if(result.items.begin(), result.items.end(),
            [](const auto& item) { return item.started; })), "Started count equals actual item callback attempts");
    }
}
int RunRecycleBinScopeTests()
{
    try
    {
        {
            Fake f; const auto scope = RB::Prepare({ L"c:\\", L"D:\\", L"C:\\", L"d:\\" }, f.Probe());
            Check(scope.Valid() && scope.GetTargets().size() == 2, "Explicit roots normalize case and deduplicate");
            Check(scope.GetTargets()[0].root == L"C:\\" && scope.GetTargets()[1].root == L"D:\\", "Selection order and exact root scope are preserved");
            auto result = RB::Execute(scope, true, f.Probe(), f.Empty());
            Check(result.state == RB::State::Succeeded && f.emptied == std::vector<std::wstring>{L"C:\\", L"D:\\"}, "Each selected root gets exactly one callback"); Counts(result);
        }
        {
            Fake f; auto scope = RB::Prepare({ L"C:\\" }, f.Probe());
            auto result = RB::Execute(scope, true, f.Probe(), f.Empty());
            Check(f.emptied == std::vector<std::wstring>{L"C:\\"} && result.started == 1, "Selecting C never invokes D or a null/all-drive target"); Counts(result);
        }
        for (const auto& invalid : { L"", L"C:", L"C:/", L"folder", L"C:\\folder", L"\\\\server\\share\\", L"\\\\?\\C:\\", L"mtp:device", L"1:\\", L" C:\\" })
        {
            Fake f; auto scope = RB::Prepare({ L"C:\\", invalid }, f.Probe());
            auto result = RB::Execute(scope, true, f.Probe(), f.Empty());
            Check(!scope.Valid() && scope.GetReason() == RB::Reason::InvalidSelection && scope.GetTargets().empty(), "Any invalid item blocks the entire selection");
            Check(f.probes.empty() && f.emptied.empty() && result.state == RB::State::Blocked, "Invalid mixed scope does not probe or execute valid prefixes"); Counts(result);
        }
        {
            Fake f; auto scope = RB::Prepare({}, f.Probe());
            Check(!scope.Valid() && RB::Execute(scope, true, f.Probe(), f.Empty()).started == 0, "Empty selection never means all drives");
            Check(!RB::Scope{}.Valid() && RB::Execute(RB::Scope{}, true, f.Probe(), f.Empty()).started == 0, "Default scope cannot forge an executable target set");
            Check(RB::Prepare({L"C:\\"}, {}).GetReason() == RB::Reason::MissingProbe, "Missing discovery callback fails closed");
            f.ids[L"C:\\"] = L""; Check(!RB::Prepare({L"C:\\"}, f.Probe()).Valid(), "Empty volume identity is rejected");
            f.ids[L"C:\\"] = L" \t"; Check(!RB::Prepare({L"C:\\"}, f.Probe()).Valid(), "Blank volume identity is rejected");
            auto missing = RB::Prepare({L"Z:\\"}, f.Probe()); Check(!missing.Valid(), "Unavailable volume identity is rejected");
            auto failed = RB::Prepare({L"C:\\"}, [](const auto&) -> std::optional<std::wstring> { throw std::runtime_error("probe"); });
            Check(failed.GetReason() == RB::Reason::ProbeFailed && failed.GetTargets().empty(), "Probe exceptions fail closed without partial scope");
        }
        {
            Fake f; const auto scope = RB::Prepare({L"C:\\", L"D:\\"}, f.Probe()); f.probes.clear();
            auto result = RB::Execute(scope, false, f.Probe(), f.Empty());
            Check(result.state == RB::State::Cancelled && result.reason == RB::Reason::NotConfirmed && f.probes.empty() && f.emptied.empty(), "No confirmation means no probe or empty call"); Counts(result);
            result = RB::Execute(scope, true, f.Probe(), f.Empty(), [] { return true; });
            Check(result.state == RB::State::Cancelled && result.started == 0 && f.emptied.empty(), "Pre-cancelled batch performs no operation"); Counts(result);
            result = RB::Execute(scope, true, f.Probe(), f.Empty(), [&] { return f.emptied.size() == 1; });
            Check(result.state == RB::State::Cancelled && result.Count(RB::State::Succeeded) == 1 && result.Count(RB::State::Cancelled) == 1 &&
                f.emptied == std::vector<std::wstring>{L"C:\\"}, "Cancellation between volumes preserves completion and skips further calls"); Counts(result);
        }
        {
            Fake f; const auto scope = RB::Prepare({L"C:\\", L"D:\\"}, f.Probe());
            f.ids[L"D:\\"] = L"replacement";
            auto result = RB::Execute(scope, true, f.Probe(), f.Empty());
            Check(result.state == RB::State::Blocked && result.reason == RB::Reason::VolumeChanged && f.emptied.empty(), "A changed second volume blocks the first before any empty call"); Counts(result);
            f.ids.erase(L"D:\\"); result = RB::Execute(scope, true, f.Probe(), f.Empty());
            Check(result.state == RB::State::Blocked && result.reason == RB::Reason::VolumeUnavailable && f.emptied.empty(), "An unavailable second volume blocks the whole batch"); Counts(result);
        }
        {
            Fake f; const auto scope = RB::Prepare({L"C:\\", L"D:\\"}, f.Probe()); size_t probes = 0;
            auto result = RB::Execute(scope, true, [&](const std::wstring& root) -> std::optional<std::wstring>
            { ++probes; return probes == 3 ? L"changed-before-first" : f.ids.at(root); }, f.Empty());
            Check(result.state == RB::State::Blocked && result.reason == RB::Reason::VolumeChanged && result.started == 0, "Per-item recheck catches replacement after full preflight"); Counts(result);
            bool cancel = false; probes = 0;
            result = RB::Execute(scope, true, [&](const std::wstring& root) -> std::optional<std::wstring>
            { if (++probes == 3) cancel = true; return f.ids.at(root); }, f.Empty(), [&] { return cancel; });
            Check(result.state == RB::State::Cancelled && result.started == 0, "Cancellation raised during per-item probe is checked before the callback"); Counts(result);
            result = RB::Execute(scope, true, [](const auto&) -> std::optional<std::wstring> { throw std::runtime_error("probe"); }, f.Empty());
            Check(result.state == RB::State::Blocked && result.reason == RB::Reason::ProbeFailed && result.started == 0, "Execution probe exceptions fail closed"); Counts(result);
            result = RB::Execute(scope, true, {}, f.Empty()); Check(result.started == 0 && result.reason == RB::Reason::MissingProbe, "Missing execution probe never empties"); Counts(result);
            result = RB::Execute(scope, true, f.Probe(), {}); Check(result.started == 0 && result.reason == RB::Reason::MissingEmpty, "Missing empty callback fails closed"); Counts(result);
            result = RB::Execute(scope, true, f.Probe(), f.Empty(), []() -> bool { throw std::runtime_error("cancel"); });
            Check(result.started == 0 && result.reason == RB::Reason::CancelCheckFailed, "Cancellation-check exception never executes"); Counts(result);
        }
        {
            Fake f; const auto scope = RB::Prepare({L"C:\\", L"D:\\", L"E:\\"}, f.Probe());
            const auto failure = static_cast<std::int32_t>(0x80070005u);
            auto result = RB::Execute(scope, true, f.Probe(), [&](const auto& root)
            { f.emptied.push_back(root); return root == L"D:\\" ? failure : std::int32_t{ 0 }; });
            Check(result.state == RB::State::Failed && result.started == 2 && result.Count(RB::State::Succeeded) == 1 && result.Count(RB::State::Failed) == 1 &&
                result.Count(RB::State::Skipped) == 1 && result.items[1].error == failure && result.items[2].reason == RB::Reason::PreviousFailure,
                "HRESULT failure stops later volumes with consistent per-item state and error");
            Check(f.emptied == std::vector<std::wstring>{L"C:\\", L"D:\\"}, "Failure never retries or invokes an unselected/global fallback"); Counts(result);
            result = RB::Execute(scope, true, f.Probe(), [](const auto&) -> std::int32_t { throw std::runtime_error("empty"); });
            Check(result.started == 1 && result.Count(RB::State::Failed) == 1 && result.Count(RB::State::Skipped) == 2, "Operation exception counts one attempt and stops the batch"); Counts(result);
            result = RB::Execute(scope, true, f.Probe(), [](const auto&) { return std::int32_t{ 1 }; });
            Check(result.state == RB::State::Succeeded && result.started == 3, "Non-negative HRESULT success codes are preserved"); Counts(result);
        }
        std::cout << "{\"case\":\"recycle_scope_mock\",\"status\":\"PASS\",\"checks\":" << checks << ",\"real_shell_calls\":0}\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
