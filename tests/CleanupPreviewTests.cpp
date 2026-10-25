// Read-only preview behavioral tests. All writes target a fresh, explicitly named fixture.
#include <windows.h>
#include <winioctl.h>
#include <sddl.h>
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "Cleanup/CleanupPreview.h"
#include "Cleanup/CleanupPlan.h"
#include "Cleanup/SpaceRecheck.h"
namespace fs = std::filesystem;
static unsigned checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
static std::uint64_t Now() { FILETIME t{}; GetSystemTimeAsFileTime(&t); return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
static void Age(const fs::path& path, std::uint64_t created, std::uint64_t written)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot set fixture dates");
    FILETIME c{DWORD(created), DWORD(created >> 32)}, w{DWORD(written), DWORD(written >> 32)};
    bool ok = SetFileTime(h, &c, nullptr, &w) != 0; CloseHandle(h);
    if (!ok) throw std::runtime_error("Cannot age fixture file");
}
static void Write(const fs::path& path, const std::string& data, std::uint64_t age)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file << data; file.close();
    if (!file) throw std::runtime_error("Cannot create fixture payload");
    Age(path, age, age);
}
static std::map<std::wstring, std::string> Snapshot(const fs::path& root)
{
    std::map<std::wstring, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file()) {
            std::ifstream stream(entry.path(), std::ios::binary);
            result.emplace(entry.path().wstring(), std::string(std::istreambuf_iterator<char>(stream), {}));
        }
    return result;
}
static bool Has(const Cleanup::ScanResult& result, const fs::path& path)
{
    const auto normalized = path.lexically_normal().make_preferred().wstring();
    for (const auto& item : result.candidates) if (_wcsicmp(item.path.c_str(), normalized.c_str()) == 0) return true;
    return false;
}
static std::uint64_t Allocation(const fs::path& path)
{
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    FILE_STANDARD_INFO info{};
    const bool ok = file != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info));
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok || info.AllocationSize.QuadPart < 0) throw std::runtime_error("Cannot query synthetic allocation");
    return static_cast<std::uint64_t>(info.AllocationSize.QuadPart);
}
static void Junction(const fs::path& link, const fs::path& target)
{
    fs::create_directories(link);
    const std::wstring substitute = L"\\??\\" + target.wstring(), print = target.wstring();
    struct Data { DWORD tag; WORD length, reserved, subOffset, subLength, printOffset, printLength; wchar_t paths[4096]; } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.subLength = WORD(substitute.size() * sizeof(wchar_t));
    data.printOffset = WORD((substitute.size() + 1) * sizeof(wchar_t));
    data.printLength = WORD(print.size() * sizeof(wchar_t));
    memcpy(data.paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    memcpy(reinterpret_cast<BYTE*>(data.paths) + data.printOffset, print.c_str(), (print.size() + 1) * sizeof(wchar_t));
    data.length = WORD(8 + data.printOffset + data.printLength + sizeof(wchar_t));
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    DWORD returned = 0;
    bool ok = h != INVALID_HANDLE_VALUE && DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, &data,
        DWORD(data.length) + 8, nullptr, 0, &returned, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    Check(ok, "Synthetic junction can be created");
}
static void CheckPlanRecords(const Cleanup::CleanupPlan& plan, const Cleanup::SimulationResult& result)
{
    using namespace Cleanup;
    Check(result.simulationOnly && result.actualReleasedBytes == 0 && result.planId == plan.Id() &&
        result.records.size() == plan.Items().size(), "Every immutable planned item has one simulation-only result");
    std::uint64_t total = 0;
    for (size_t i = 0; i < result.records.size(); ++i) {
        const auto& record = result.records[i];
        const auto& item = plan.Items()[i];
        const bool passed = record.state == SimulationState::Simulated;
        Check(record.itemIndex == i && (passed == (record.reason == PlanReason::None)) &&
            (!passed || record.systemError == 0), "Per-item indexes, state, reason and error agree");
        Check(record.state != SimulationState::Cancelled ||
            (record.reason == PlanReason::Cancelled && record.systemError == 0), "Cancellation carries no stale file error");
        Check(record.estimatedReleaseUpperBound == (passed && item.disposition == Disposition::PermanentDelete ?
            item.candidate.allocatedBytes : 0), "Only passed permanent intent contributes an allocation upper bound");
        total += record.estimatedReleaseUpperBound;
    }
    Check(total == result.estimatedReleaseUpperBound, "Summary upper bound equals the sum of item results");
}
static void RunPlanTests(const fs::path& root, std::uint64_t now, std::uint64_t old)
{
    using namespace Cleanup;
    const auto fixture = root/L"PlanTests", local = fixture/L"LocalAppData", temp = local/L"Temp";
    const auto first = temp/L"first.tmp", second = temp/L"second.tmp";
    Write(first, std::string(8193, 'a'), old);
    Write(second, std::string(4097, 'b'), old);
    Write(temp/L"personal.txt", "personal sentinel", old);
    ScanOptions options; options.localAppData = local.wstring(); options.now = now;
    options.enabled.fill(false); options.enabled[0] = true;
    auto preview = Scan(options);
    Check(!preview.incomplete && preview.candidates.size() == 2, "Plan fixture has exactly two eligible files");
    std::vector<bool> selected(preview.candidates.size(), false);
    const auto firstIndex = static_cast<size_t>(std::find_if(preview.candidates.begin(), preview.candidates.end(),
        [&](const Candidate& candidate) { return candidate.path == first.wstring(); }) - preview.candidates.begin());
    Check(firstIndex < selected.size(), "Plan selection resolves the intended candidate identity");
    selected[firstIndex] = true;
    const auto beforeBuild = Snapshot(fixture);
    auto plan = BuildPlan(preview, selected, options);
    Check(plan.Reason() == PlanReason::None && plan.Items().size() == 1 && !plan.Id().empty(),
        "Only explicitly checked files enter a named independent plan");
    Check(plan.Items()[0].candidate.path == first.wstring() && plan.Items()[0].disposition == Disposition::RecycleBin &&
        plan.Items()[0].candidate.ruleId == L"user-temp-v1" && plan.Items()[0].candidate.ruleVersion == 1 &&
        !plan.Items()[0].candidate.reasonKey.empty(), "Plan retains file, versioned rule evidence and explicit recycle disposition");
    Check(plan.EstimatedReleaseUpperBound() == 0 && Snapshot(fixture) == beforeBuild,
        "Building a recycle plan preserves all source bytes and does not claim released space");
    // Every simulation is bracketed by complete path/content snapshots, including failure cases.
    const auto simulate = [&](const CleanupPlan& input, const ScanOptions& current, std::stop_token stop = {}) {
        const auto before = Snapshot(fixture);
        auto result = SimulatePlan(input, current, stop);
        CheckPlanRecords(input, result);
        Check(Snapshot(fixture) == before, "Simulation preserves every synthetic path and default-stream byte");
        Check(result.simulationOnly && result.actualReleasedBytes == 0 && result.planId == input.Id() &&
            result.finishedAt >= result.startedAt && result.records.size() == input.Items().size(),
            "Simulation reports its plan, zero actual release, timestamps and one record per planned item");
        for (size_t i = 0; i < result.records.size(); ++i)
            Check(result.records[i].itemIndex == i && result.records[i].finishedAt >= result.records[i].startedAt,
                "Operation records retain stable item indexes and ordered timestamps");
        return result;
    };
    const auto expectSkip = [&](const CleanupPlan& input, const ScanOptions& current, PlanReason reason) {
        const auto result = simulate(input, current);
        if (result.records.size() != 1 || result.records[0].state != SimulationState::Skipped ||
            result.records[0].reason != reason || result.estimatedReleaseUpperBound != 0)
            std::cerr << "Skip diagnostic: expected=" << static_cast<unsigned>(reason) << " plan=" << static_cast<unsigned>(input.Reason()) <<
                " items=" << input.Items().size() << " records=" << result.records.size() <<
                " actual=" << (result.records.empty() ? 255 : static_cast<unsigned>(result.records[0].reason)) << std::endl;
        Check(result.records.size() == 1 && result.records[0].state == SimulationState::Skipped &&
            result.records[0].reason == reason && result.estimatedReleaseUpperBound == 0,
            "Failed safety condition skips its item with the expected reason and no release estimate");
    };
    auto simulated = simulate(plan, options);
    Check(simulated.records[0].state == SimulationState::Simulated && simulated.records[0].reason == PlanReason::None &&
        simulated.estimatedReleaseUpperBound == 0, "Unchanged recycle item is simulated without claiming physical release");
    auto permanent = BuildPlan(preview, selected, options, Disposition::PermanentDelete);
    Check(permanent.Id() != plan.Id(), "Independent plans have distinct correlation identifiers");
    simulated = simulate(permanent, options);
    Check(permanent.Items()[0].disposition == Disposition::PermanentDelete &&
        simulated.records[0].state == SimulationState::Simulated &&
        permanent.EstimatedReleaseUpperBound() == Allocation(first) && simulated.estimatedReleaseUpperBound == Allocation(first),
        "Explicit permanent-delete intent estimates allocation only as an upper bound and performs no deletion");
    auto official = BuildPlan(preview, selected, options, Disposition::WindowsCleanup);
    expectSkip(official, options, PlanReason::UnsupportedDisposition);
    Check(official.Items()[0].disposition == Disposition::WindowsCleanup,
        "Unsupported Windows cleanup keeps its requested disposition without fallback");
    auto changedOptions = options; changedOptions.excludedDirectories = { temp.wstring() };
    expectSkip(permanent, changedOptions, PlanReason::ConfigurationChanged);
    changedOptions = options; changedOptions.protectedDirectories = { temp.wstring() };
    expectSkip(permanent, changedOptions, PlanReason::ConfigurationChanged);
    changedOptions = options; changedOptions.enabled[0] = false;
    expectSkip(permanent, changedOptions, PlanReason::ConfigurationChanged);
    changedOptions = options; changedOptions.localAppData = (fixture/L"OtherLocal").wstring();
    expectSkip(permanent, changedOptions, PlanReason::ConfigurationChanged);
    std::stop_source stop; stop.request_stop();
    const auto cancelledBuild = BuildPlan(preview, selected, options, Disposition::PermanentDelete, stop.get_token());
    Check(cancelledBuild.Reason() == PlanReason::Cancelled && cancelledBuild.EstimatedReleaseUpperBound() == 0 &&
        cancelledBuild.Items()[0].reason == PlanReason::Cancelled, "Cancelled plan generation stops metadata checks and cannot become usable");
    simulated = simulate(permanent, options, stop.get_token());
    Check(simulated.records[0].state == SimulationState::Cancelled && simulated.records[0].reason == PlanReason::Cancelled &&
        simulated.estimatedReleaseUpperBound == 0, "Cancellation records every unstarted item without retry or release");
    auto invalid = preview; invalid.incomplete = true;
    Check(BuildPlan(invalid, selected, options).Reason() == PlanReason::IncompletePreview,
        "Incomplete preview cannot produce a usable cleanup plan");
    invalid = preview; invalid.cancelled = true;
    Check(BuildPlan(invalid, selected, options).Reason() == PlanReason::IncompletePreview,
        "Cancelled preview cannot become a complete plan");
    invalid = preview; invalid.limitReached = true;
    Check(BuildPlan(invalid, selected, options).Reason() == PlanReason::IncompletePreview,
        "Truncated preview cannot become a complete plan");
    Check(BuildPlan(preview, std::vector<bool>(selected.size(), false), options).Reason() == PlanReason::InvalidSelection &&
        BuildPlan(preview, {}, options).Reason() == PlanReason::InvalidSelection &&
        BuildPlan(preview, selected, options, static_cast<Disposition>(255)).Reason() == PlanReason::InvalidSelection,
        "Empty, mismatched and invalid-disposition requests fail closed");
    invalid = preview; ++invalid.candidates[firstIndex].ruleVersion;
    expectSkip(BuildPlan(invalid, selected, options), options, PlanReason::RuleChanged);
    invalid = preview; invalid.candidates[firstIndex].ruleId = L"unregistered-rule";
    expectSkip(BuildPlan(invalid, selected, options), options, PlanReason::RuleChanged);
    invalid = preview; invalid.candidates.push_back(invalid.candidates[firstIndex]);
    auto duplicateSelection = selected; duplicateSelection.push_back(true);
    const auto duplicate = BuildPlan(invalid, duplicateSelection, options, Disposition::PermanentDelete);
    simulated = simulate(duplicate, options);
    Check(simulated.records.size() == 2 && simulated.records[1].reason == PlanReason::Duplicate &&
        simulated.records[1].state == SimulationState::Skipped && simulated.estimatedReleaseUpperBound == Allocation(first),
        "Repeated physical identity cannot inflate the release upper bound");
    // Source vectors and later discovered files do not alter the immutable plan.
    selected.assign(selected.size(), false); preview.candidates.clear();
    Write(temp/L"newly-discovered.tmp", "not in the old plan", old);
    simulated = simulate(permanent, options);
    Check(permanent.Items().size() == 1 && simulated.records.size() == 1 && permanent.Items()[0].candidate.path == first.wstring(),
        "Changing selection and discovery results never adds new files to the old plan");
    std::vector<bool> all(Scan(options).candidates.size(), true);
    auto partial = BuildPlan(Scan(options), all, options, Disposition::PermanentDelete);
    Write(first, std::string(8193, 'z'), old); // Same length and restored visible dates; ChangeTime must still invalidate it.
    expectSkip(permanent, options, PlanReason::FileChanged);
    simulated = simulate(partial, options);
    Check(std::count_if(simulated.records.begin(), simulated.records.end(), [](const OperationRecord& record) {
        return record.state == SimulationState::Simulated; }) == 2 &&
        std::count_if(simulated.records.begin(), simulated.records.end(), [](const OperationRecord& record) {
            return record.reason == PlanReason::FileChanged; }) == 1,
        "One changed file is skipped while unchanged planned items retain independent results");
    const auto makeSingle = [&](const wchar_t* name) {
        const auto path = (temp/name).make_preferred(); Write(path, "original payload", old);
        auto current = Scan(options); std::vector<bool> choice(current.candidates.size(), false);
        for (size_t i = 0; i < choice.size(); ++i) choice[i] = current.candidates[i].path == path.wstring();
        Check(std::count(choice.begin(), choice.end(), true) == 1, "Single-file plan resolves exactly one normalized synthetic candidate");
        return BuildPlan(current, choice, options, Disposition::PermanentDelete);
    };
    auto replaced = makeSingle(L"replacement.tmp");
    fs::rename(temp/L"replacement.tmp", temp/L"replacement.kept");
    Write(temp/L"replacement.tmp", "original payload", old);
    expectSkip(replaced, options, PlanReason::IdentityChanged);
    auto missing = makeSingle(L"missing.tmp");
    fs::rename(temp/L"missing.tmp", temp/L"missing.kept");
    expectSkip(missing, options, PlanReason::Unavailable);
    auto streamPlan = makeSingle(L"streams.tmp");
    { std::ofstream stream(temp/L"streams.tmp:user-data", std::ios::binary); stream << "named stream sentinel"; }
    simulated = simulate(streamPlan, options);
    Check(simulated.records[0].state == SimulationState::Skipped,
        "A named stream added after planning prevents simulated eligibility");
    { std::ifstream stream(temp/L"streams.tmp:user-data", std::ios::binary);
      Check(std::string(std::istreambuf_iterator<char>(stream), {}) == "named stream sentinel", "Simulation preserves named-stream bytes"); }
    auto linked = makeSingle(L"hardlinked.tmp");
    Check(CreateHardLinkW((temp/L"hardlinked.kept").c_str(), (temp/L"hardlinked.tmp").c_str(), nullptr) != 0,
        "Plan test creates a synthetic hardlink without deleting any payload");
    simulated = simulate(linked, options);
    Check(simulated.records[0].state == SimulationState::Skipped, "A hardlink added after planning invalidates eligibility");
    auto locked = makeSingle(L"locked.tmp");
    const auto beforeLock = Snapshot(fixture);
    HANDLE lock = CreateFileW((temp/L"locked.tmp").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(lock != INVALID_HANDLE_VALUE, "Synthetic file can be exclusively locked");
    // Snapshot cannot open the deliberately locked payload; compare complete snapshots after releasing the lock.
    const auto lockedResult = SimulatePlan(locked, options);
    CloseHandle(lock);
    Check(Snapshot(fixture) == beforeLock, "The sharing-conflict simulation preserves all fixture bytes and paths");
    Check(lockedResult.records.size() == 1 && lockedResult.records[0].state == SimulationState::Skipped &&
        lockedResult.records[0].systemError != 0 && lockedResult.actualReleasedBytes == 0,
        "A sharing conflict is recorded without taking ownership, changing permissions or retrying");
    auto repository = makeSingle(L"repository-case/payload.tmp");
    Write(temp/L"repository-case/.git", "repository marker", old);
    expectSkip(repository, options, PlanReason::NoLongerEligible);
    const auto linkedDirectory = temp/L"redirected", preservedDirectory = temp/L"redirected-kept", outside = fixture/L"outside";
    Write(linkedDirectory/L"payload.tmp", "planned original", old); Write(outside/L"payload.tmp", "protected sentinel", old);
    auto current = Scan(options); std::vector<bool> choice(current.candidates.size(), false);
    for (size_t i = 0; i < choice.size(); ++i) choice[i] = current.candidates[i].path == (linkedDirectory/L"payload.tmp").wstring();
    auto redirected = BuildPlan(current, choice, options, Disposition::PermanentDelete);
    fs::rename(linkedDirectory, preservedDirectory); Junction(linkedDirectory, outside);
    expectSkip(redirected, options, PlanReason::UnsafePath);
    Check(Snapshot(outside).at((outside/L"payload.tmp").wstring()) == "protected sentinel" &&
        Snapshot(preservedDirectory).at((preservedDirectory/L"payload.tmp").wstring()) == "planned original",
        "Ancestor redirection never changes the outside sentinel or the retained original");
}
static void RunPlanGuardTests(const fs::path& root, std::uint64_t now, std::uint64_t old)
{
    using namespace Cleanup;
    const auto fixture = root/L"PlanGuards", local = fixture/L"LocalAppData", temp = local/L"Temp";
    for (const auto* name : { L"a", L"b", L"c" }) Write(temp/name/L"payload.tmp", std::string(8193, 'x'), old);
    ScanOptions options; options.localAppData = local.wstring(); options.now = now;
    options.enabled.fill(false); options.enabled[0] = true;
    const auto build = [&]() {
        const auto scanned = Scan(options);
        Check(!scanned.incomplete && scanned.candidates.size() == 3, "Guard fixture has three complete eligible candidates");
        return BuildPlan(scanned, std::vector<bool>(3, true), options, Disposition::PermanentDelete);
    };
    auto plan = build();
    const auto before = Snapshot(fixture);
    auto result = SimulatePlan(plan, options);
    CheckPlanRecords(plan, result);
    Check(std::all_of(result.records.begin(), result.records.end(), [](const auto& record) {
        return record.state == SimulationState::Simulated; }), "All unchanged guard items pass metadata observation");

    // The writer itself permits other readers; the verifier must still reject its write access.
    HANDLE writer = CreateFileW(plan.Items()[1].candidate.path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(writer != INVALID_HANDLE_VALUE, "A synthetic ordinary writer allows sharing without writing any bytes");
    try { result = SimulatePlan(plan, options); } catch (...) { CloseHandle(writer); throw; }
    CloseHandle(writer);
    CheckPlanRecords(plan, result);
    Check(result.records[1].state == SimulationState::Skipped && result.records[1].reason == PlanReason::Unavailable &&
        result.records[1].systemError == ERROR_SHARING_VIOLATION &&
        result.records[0].state == SimulationState::Simulated && result.records[2].state == SimulationState::Simulated,
        "A shared writer causes one explicit sharing skip while independent items continue");

    // Save and restore only the new synthetic file's DACL, including its inheritance state.
    const auto deniedPath = plan.Items()[1].candidate.path;
    DWORD securitySize = 0;
    GetFileSecurityW(deniedPath.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &securitySize);
    std::vector<BYTE> security(securitySize);
    Check(securitySize != 0 && GetFileSecurityW(deniedPath.c_str(), DACL_SECURITY_INFORMATION,
        security.data(), securitySize, &securitySize), "Synthetic file DACL saved before changing read permission");
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    Check(GetSecurityDescriptorControl(security.data(), &control, &revision) != 0, "Synthetic DACL inheritance captured");
    PSECURITY_DESCRIPTOR denied = nullptr;
    Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)",
        SDDL_REVISION_1, &denied, nullptr) != 0, "Synthetic deny-read descriptor prepared");
    const bool applied = SetFileSecurityW(deniedPath.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, denied) != 0;
    LocalFree(denied);
    Check(applied, "Read access revoked only on the synthetic file after planning");
    const auto restore = [&]() { return SetFileSecurityW(deniedPath.c_str(), DACL_SECURITY_INFORMATION |
        ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION), security.data()) != 0; };
    try { result = SimulatePlan(plan, options); } catch (...) { restore(); throw; }
    Check(restore(), "Synthetic read permission and inheritance restored without elevation");
    CheckPlanRecords(plan, result);
    Check(result.records[1].state == SimulationState::Skipped && result.records[1].reason == PlanReason::Unavailable &&
        result.records[1].systemError == ERROR_ACCESS_DENIED && result.records[0].state == SimulationState::Simulated &&
        result.records[2].state == SimulationState::Simulated, "Revoked read permission yields access denied and partial results");
    Check(Snapshot(fixture) == before, "Shared-writer and ACL tests preserve every synthetic payload");

    plan = build();
    std::stop_source stop;
    size_t notifications = 0;
    result = SimulatePlan(plan, options, stop.get_token(), [&](const OperationRecord& record) {
        ++notifications; if (record.itemIndex == 0) stop.request_stop();
    });
    CheckPlanRecords(plan, result);
    Check(notifications == 3 && result.records[0].state == SimulationState::Simulated &&
        result.records[1].state == SimulationState::Cancelled && result.records[2].state == SimulationState::Cancelled,
        "Cancellation after the first result prevents later observations and retains all result rows");
    const auto scanned = Scan(options);
    const auto cancelledPlan = BuildPlan(scanned, std::vector<bool>(3, true), options, Disposition::PermanentDelete, stop.get_token());
    result = SimulatePlan(cancelledPlan, options);
    CheckPlanRecords(cancelledPlan, result);
    Check(std::all_of(result.records.begin(), result.records.end(), [](const auto& record) {
        return record.state == SimulationState::Cancelled; }), "A cancelled plan stays cancelled even with a fresh token");
    auto excluded = options; excluded.excludedDirectories = { fs::path(plan.Items()[1].candidate.path).parent_path().wstring() };
    result = SimulatePlan(plan, excluded);
    CheckPlanRecords(plan, result);
    Check(std::all_of(result.records.begin(), result.records.end(), [](const auto& record) {
        return record.reason == PlanReason::ConfigurationChanged; }), "An exclusion change invalidates the complete old plan");
    auto changing = options;
    result = SimulatePlan(plan, changing, {}, [&](const OperationRecord& record) {
        if (record.itemIndex == 0) changing.excludedDirectories = excluded.excludedDirectories;
        if (record.itemIndex == 1) changing.excludedDirectories.clear();
    });
    CheckPlanRecords(plan, result);
    Check(result.records[0].state == SimulationState::Simulated &&
        result.records[1].reason == PlanReason::ConfigurationChanged && result.records[2].reason == PlanReason::ConfigurationChanged,
        "An exclusion change during simulation stops all remaining items even if reverted later");

    // Deterministic changes after the fresh eligibility scan, before the next item is checked.
    auto expected = Snapshot(fixture);
    result = SimulatePlan(plan, options, {}, [&](const OperationRecord& record) {
        if (record.itemIndex != 0) return;
        const fs::path path = plan.Items()[1].candidate.path;
        fs::rename(path, path.wstring() + L".kept");
        Write(path, std::string(8193, 'y'), old);
        expected = Snapshot(fixture);
    });
    CheckPlanRecords(plan, result);
    Check(result.records[1].reason == PlanReason::IdentityChanged &&
        result.records[0].state == SimulationState::Simulated && result.records[2].state == SimulationState::Simulated,
        "A same-name replacement after the eligibility scan is rejected by file identity");
    Check(Snapshot(fixture) == expected, "Simulation preserves both the retained original and the replacement");

    plan = build();
    result = SimulatePlan(plan, options, {}, [&](const OperationRecord& record) {
        if (record.itemIndex == 0) Age(plan.Items()[1].candidate.path, old, old - 10000000);
    });
    CheckPlanRecords(plan, result);
    Check(result.records[1].reason == PlanReason::FileChanged,
        "A metadata-only change after eligibility is rejected even though the file is still old enough");

    plan = build();
    const auto redirectedPath = fs::path(plan.Items()[1].candidate.path).parent_path();
    const auto retained = fs::path(redirectedPath.wstring() + L"-kept"), outside = fixture/L"outside";
    Write(outside/L"payload.tmp", "outside synthetic sentinel", old);
    result = SimulatePlan(plan, options, {}, [&](const OperationRecord& record) {
        if (record.itemIndex != 0) return;
        fs::rename(redirectedPath, retained); Junction(redirectedPath, outside);
        expected = Snapshot(fixture);
    });
    CheckPlanRecords(plan, result);
    Check(result.records[1].reason == PlanReason::UnsafePath &&
        result.records[0].state == SimulationState::Simulated && result.records[2].state == SimulationState::Simulated,
        "An ancestor junction substituted after eligibility is skipped without touching its target");
    Check(Snapshot(fixture) == expected, "Redirection checks preserve the sentinel and retained directory contents");
}
static void SpaceRecheckMocks()
{
    using namespace Cleanup::SpaceRecheck;
    Snapshot baseline;
    Check(!Comparable(baseline, baseline), "Unmeasured snapshots cannot be compared");
    baseline.state = State::Ready;
    baseline.root = L"C:\\";
    baseline.directory = L"C:\\synthetic";
    baseline.volumeId = L"synthetic-volume|device";
    baseline.directoryId = L"1:0:123";
    baseline.capacity = 1000;
    baseline.available = 100;
    baseline.logical = 200;
    baseline.allocated = 256;
    Check(Comparable(baseline, baseline), "Same successful observation target compares");
    auto after = baseline;
    after.available = 300;
    after.logical = 400;
    Check(Comparable(baseline, after), "Free-space increase can coincide with directory growth");
    after.available = 0;
    Check(Comparable(baseline, after), "Free-space decrease remains a valid observation");
    after.available = baseline.available;
    Check(Comparable(baseline, after), "Zero available-space change remains valid");
    for (auto state : { State::Failed, State::Cancelled, State::Changed })
    {
        after = baseline; after.state = state;
        Check(!Comparable(baseline, after), "Failed, cancelled or changed result cannot compare");
        Check(!Comparable(after, baseline), "Invalid baseline cannot compare");
    }
    after = baseline; after.root = L"D:\\";
    Check(!Comparable(baseline, after), "Different drive refused");
    after = baseline; after.volumeId += L"-replacement";
    Check(!Comparable(baseline, after), "Replaced volume refused");
    after = baseline; after.volumeId.clear();
    Check(!Comparable(baseline, after), "Unknown volume refused");
    after = baseline; ++after.capacity;
    Check(!Comparable(baseline, after), "Changed capacity refused");
    after = baseline; after.directory += L"-sibling";
    Check(!Comparable(baseline, after), "Changed selected directory refused");
    after = baseline; after.directoryId += L"-replacement";
    Check(!Comparable(baseline, after), "Replaced selected directory refused");
    after = baseline; after.directoryComplete = false;
    Check(Comparable(baseline, after), "Partial directory does not lose volume observation");
    after = baseline; after.directoryId.clear(); after.directoryComplete = false;
    Check(Comparable(baseline, after), "Missing directory identity still allows only volume observation");
    after = baseline; after.directory.clear(); after.directoryId.clear();
    Check(!Comparable(baseline, after), "Switching from a directory to drive-only invalidates baseline");
    Check(baseline.available == 100 && baseline.logical == 200, "Rechecks preserve baseline");
}
int wmain(int argc, wchar_t** argv)
{
    try {
        SpaceRecheckMocks();
        if (argc == 2 && std::wstring(argv[1]) == L"--space-recheck") {
            std::cout << "{\"space_recheck_mock_checks\":" << checks << ",\"real_cleanup_calls\":0}\n"; return 0;
        }
        if (argc != 2) throw std::runtime_error("Pass one fresh synthetic fixture path");
        const fs::path root = fs::path(argv[1]).lexically_normal();
        if (!root.is_absolute() || root.filename().wstring().find(L"WinDirStatCleanupTests-") != 0 || fs::exists(root))
            throw std::runtime_error("Refusing an existing or unowned fixture path");
        fs::create_directories(root);
        const auto now = Now(), day = std::uint64_t(86400) * 10000000, old = now - 10 * day;
        const auto local = root / L"LocalAppData", temp = local / L"Temp", roaming = root / L"RoamingAppData";
        const auto chrome = local / L"Google/Chrome/User Data/Default/Cache/Cache_Data";
        const auto edge = local / L"Microsoft/Edge/User Data/Profile 1/Cache/Cache_Data";
        const auto firefox = local / L"Mozilla/Firefox/Profiles/test.default/cache2/entries";
        const auto pip = local / L"pip/Cache/http-v2/a/b/c/d/e";
        const auto brave = local / L"BraveSoftware/Brave-Browser/User Data/Default/Cache/Cache_Data";
        const auto vivaldi = local / L"Vivaldi/User Data/Profile 2/Cache/Cache_Data";
        const auto code = roaming / L"Code/CachedData/0123456789abcdef0123456789abcdef01234567";
        const auto npm = local / L"npm-cache/_cacache/content-v2/sha512/aa";
        const auto nuget = local / L"NuGet/v3-cache/source";
        const auto legacyPip = local / L"pip/Cache/http/a/b/c/d/e";
        std::vector<fs::path> expected{ temp/L"old.tmp", temp/L"nested/old.temp", chrome/L"cache.bin",
            edge/L"cache.bin", firefox/L"0123456789ABCDEF0123456789ABCDEF01234567", pip/L"response.body",
            brave/L"cache.bin", vivaldi/L"cache.bin", code/L"compiled.bin", npm/L"payload", nuget/L"list.dat", legacyPip/L"response.body" };
        for (size_t i = 0; i < expected.size(); ++i) Write(expected[i], std::string(4097 + i * 17, char('a' + i)), old);
        Age(code/L"compiled.bin", now - 100 * day, now - 100 * day);
        expected.push_back(temp/L"zero.tmp"); Write(expected.back(), "", old);
        expected.push_back(temp/L"resident.tmp"); Write(expected.back(), "tiny", old);
        Write(temp/L"recent.tmp", "recent", now);
        Write(temp/L"new-created.tmp", "created recently", old); Age(temp/L"new-created.tmp", now, old);
        Write(temp/L"new-written.tmp", "written recently", old); Age(temp/L"new-written.tmp", old, now);
        Write(temp/L"personal.docx", "personal document", old);
        Write(local/L"Google/Chrome/User Data/Default/Cookies", "credential sentinel", old);
        Write(local/L"Google/Chrome/User Data/Default/Service Worker/CacheStorage/sentinel", "offline data", old);
        Write(local/L"pip/Cache/wheels/package.whl", "offline wheel", old);
        Write(local/L"UnknownApp/Cache/unknown.bin", "unknown purpose", old);
        Write(local/L"Google/Chrome/User Data/Custom/Cache/Cache_Data/unknown.bin", "unknown profile", old);
        Write(local/L"BraveSoftware/Brave-Browser/User Data/Default/Local Storage/wallet", "wallet sentinel", old);
        Write(local/L"Vivaldi/User Data/Profile 2/Cookies", "cookie sentinel", old);
        Write(local/L"npm-cache/_npx/project/node_modules/package/index.js", "execution cache outside whitelist", old);
        Write(local/L"npm-cache/.npmrc", "credential sentinel", old);
        Write(local/L"NuGet/plugins-cache/sentinel", "plugin cache outside whitelist", old);
        Write(root/L".nuget/packages/project/package.dll", "direct project dependency", old);
        Write(roaming/L"Code/User/settings.json", "user settings", old);
        Write(roaming/L"Code/Backups/unsaved.txt", "unsaved work", old);
        Write(roaming/L"Code/User/workspaceStorage/history", "workspace user data", old);
        Write(roaming/L"Code/CachedData/not-a-commit/personal.txt", "unknown folder", now - 100 * day);
        Write(code/L"ten-days.bin", "not old enough for code cache", old);
        auto before = Snapshot(root);
        Cleanup::ScanOptions options; options.localAppData = local.wstring(); options.roamingAppData = roaming.wstring(); options.now = now;
        auto result = Cleanup::Scan(options);
        Check(!result.incomplete && !result.cancelled, "Normal preview is complete");
        Check(result.candidates.size() == expected.size(), "Only audited old temporary/cache payloads are candidates");
        for (size_t i = 0; i < result.coverage.size(); ++i) {
            const auto& category = result.coverage[i];
            Check(category.state == Cleanup::CoverageState::Matches && category.rootsFound == (i == static_cast<size_t>(Cleanup::Category::Pip) ? 2 : 1) && category.failures == 0,
                "Each audited category reports an existing exact cache root and matching candidates");
        }
        for (const auto& path : expected) Check(Has(result, path), "Expected precise whitelist payload found");
        std::uint64_t count = 0, allocation = 0;
        for (const auto& category : result.categories) { count += category.files; allocation += category.allocatedBytes; }
        Check(count == result.candidates.size(), "Category totals do not double count files");
        std::uint64_t candidateAllocation = 0;
        for (const auto& item : result.candidates) {
            Check(!item.ruleId.empty() && !item.reasonKey.empty() && item.ruleVersion != 0, "Candidate has versioned matching reason");
            Check(item.allocatedBytes == Allocation(item.path), "Candidate allocation matches filesystem metadata including resident and empty files");
            Check(item.assessment == (item.category == Cleanup::Category::Temporary || item.category == Cleanup::Category::VSCode ?
                Cleanup::Assessment::ManualReview : Cleanup::Assessment::RegenerableCache),
                "Regenerable caches are distinct from temporary and compiled-cache candidates requiring judgment");
            candidateAllocation += item.allocatedBytes;
        }
        Check(candidateAllocation == allocation, "Category allocation equals unique candidate allocation");
        Check(Snapshot(root) == before, "Read-only preview preserves all source bytes and paths");
        auto restricted = options; restricted.enabled.fill(false); restricted.enabled[0] = true;
        auto onlyTemp = Cleanup::Scan(restricted);
        Check(onlyTemp.candidates.size() == 4, "Category selection limits scanning");
        Check(onlyTemp.coverage[1].state == Cleanup::CoverageState::Disabled, "Disabled categories do not claim to be missing");
        restricted.enabled.fill(false);
        Check(Cleanup::Scan(restricted).candidates.empty(), "No categories yields no candidates");
        Write(temp/L"excluded/item.tmp", "excluded", old);
        Write(temp/L"excluded-sibling/item.tmp", "sibling", old);
        restricted = options; restricted.excludedDirectories = { (temp/L"excluded").wstring() };
        result = Cleanup::Scan(restricted);
        Check(!Has(result, temp/L"excluded/item.tmp"), "Excluded directory wins over builtin rules");
        Check(Has(result, temp/L"excluded-sibling/item.tmp"), "Directory prefix sibling remains eligible");
        restricted = options; restricted.protectedDirectories = { temp.wstring() };
        result = Cleanup::Scan(restricted);
        Check(!Has(result, temp/L"old.tmp") && Has(result, expected[2]), "Protected roots override a matching rule without hiding other categories");
        Check(result.coverage[0].state == Cleanup::CoverageState::Skipped, "Excluded roots do not claim there are no matching files");
        const auto emptyLocal = root/L"EmptyLocalAppData", emptyRoaming = root/L"EmptyRoamingAppData";
        fs::create_directories(emptyLocal);
        fs::create_directories(emptyRoaming);
        restricted = options; restricted.localAppData = emptyLocal.wstring(); restricted.roamingAppData = emptyRoaming.wstring();
        result = Cleanup::Scan(restricted);
        for (size_t i = 0; i < result.coverage.size(); ++i) {
            const auto& category = result.coverage[i];
            Check(category.state == Cleanup::CoverageState::Missing && category.missingRoots == (i == static_cast<size_t>(Cleanup::Category::Pip) ? 2 : 1),
                "Missing audited directories are distinct from an empty scan");
        }
        for (const auto& relative : { L"Temp", L"Google/Chrome/User Data/Default/Cache/Cache_Data",
            L"Microsoft/Edge/User Data/Profile 2/Cache/Cache_Data", L"Mozilla/Firefox/Profiles/empty.default/cache2/entries", L"pip/Cache/http-v2", L"pip/Cache/http",
            L"BraveSoftware/Brave-Browser/User Data/Default/Cache/Cache_Data", L"Vivaldi/User Data/Default/Cache/Cache_Data", L"npm-cache/_cacache", L"NuGet/v3-cache" })
            fs::create_directories(emptyLocal/relative);
        fs::create_directories(emptyRoaming/L"Code/CachedData/0123456789abcdef0123456789abcdef01234567");
        Write(emptyLocal/L"Temp/recent.tmp", "recent", now);
        result = Cleanup::Scan(restricted);
        for (size_t i = 0; i < result.coverage.size(); ++i) {
            const auto& category = result.coverage[i];
            Check(category.state == Cleanup::CoverageState::NoMatches && category.rootsFound == (i == static_cast<size_t>(Cleanup::Category::Pip) ? 2 : 1) && category.failures == 0,
                "Existing empty or ineligible cache roots report no matching files without scan failure");
        }
        restricted = options; restricted.roamingAppData = (root/L"MissingRoamingAppData").wstring();
        result = Cleanup::Scan(restricted);
        Check(result.incomplete && Has(result, temp/L"old.tmp") && !Has(result, code/L"compiled.bin") &&
            result.coverage[static_cast<size_t>(Cleanup::Category::VSCode)].state == Cleanup::CoverageState::Failed &&
            result.coverage[0].state == Cleanup::CoverageState::Matches,
            "Unavailable roaming anchor fails only its category without losing successful local candidates");
        restricted = options; restricted.protectedDirectories = { roaming.wstring() };
        result = Cleanup::Scan(restricted);
        Check(!Has(result, code/L"compiled.bin") && Has(result, brave/L"cache.bin") &&
            result.coverage[static_cast<size_t>(Cleanup::Category::VSCode)].state == Cleanup::CoverageState::Skipped,
            "Protected roaming anchor wins over a cache rule without suppressing local categories");
        // Missing one pip layout must not erase matches from the other layout.
        restricted = options; restricted.localAppData = (root/L"LegacyOnly").wstring();
        Write(fs::path(restricted.localAppData)/L"pip/Cache/http/entry", "legacy only", old);
        result = Cleanup::Scan(restricted);
        Check(result.coverage[static_cast<size_t>(Cleanup::Category::Pip)].state == Cleanup::CoverageState::Matches &&
            result.categories[static_cast<size_t>(Cleanup::Category::Pip)].files == 1,
            "Legacy HTTP matches and a missing modern pip directory share one accurate category");
        const auto overlapLocal = root/L"OverlapLocal", overlapRoaming = overlapLocal/L"Temp/Roaming";
        const auto overlapFile = overlapRoaming/L"Code/CachedData/0123456789abcdef0123456789abcdef01234567/cache.tmp";
        Write(overlapFile, "one physical file seen by two rules", now - 100 * day);
        restricted = options; restricted.localAppData = overlapLocal.wstring(); restricted.roamingAppData = overlapRoaming.wstring();
        result = Cleanup::Scan(restricted);
        Check(result.candidates.size() == 1 && result.skipped[static_cast<size_t>(Cleanup::SkipReason::Duplicate)] == 1,
            "Overlapping audited roots still count the same physical file only once across anchors");
        Check(result.categories[0].allocatedBytes == Allocation(overlapFile) &&
            result.categories[static_cast<size_t>(Cleanup::Category::VSCode)].allocatedBytes == 0,
            "Cross-anchor deduplication does not inflate category allocation or the release upper bound");
        restricted = options; restricted.enabled.fill(false);
        for (const auto category : { Cleanup::Category::Pip, Cleanup::Category::Brave, Cleanup::Category::Vivaldi })
            restricted.enabled[static_cast<size_t>(category)] = true;
        restricted.maximumCandidates = 2;
        result = Cleanup::Scan(restricted);
        Check(result.limitReached && result.coverage[static_cast<size_t>(Cleanup::Category::Pip)].state == Cleanup::CoverageState::Partial &&
            result.coverage[static_cast<size_t>(Cleanup::Category::Vivaldi)].state == Cleanup::CoverageState::NotScanned,
            "A limit between modern and legacy pip rules marks their shared category partial, leaving untouched categories unscanned");
        restricted.maximumCandidates = 50000;
        std::stop_source betweenPipRules;
        result = Cleanup::Scan(restricted, betweenPipRules.get_token(), [&](const Cleanup::Progress& progress) {
            if (progress.category == Cleanup::Category::Brave) betweenPipRules.request_stop();
        });
        Check(result.cancelled && Has(result, pip/L"response.body") && !Has(result, legacyPip/L"response.body") &&
            result.coverage[static_cast<size_t>(Cleanup::Category::Pip)].state == Cleanup::CoverageState::Partial &&
            result.coverage[static_cast<size_t>(Cleanup::Category::Vivaldi)].state == Cleanup::CoverageState::NotScanned,
            "Cancellation between layouts never claims that the entire pip category was scanned");
        // A file's attributes may remain readable through its parent directory.
        // Deny directory enumeration to create a deterministic read failure.
        const auto deniedPath = temp/L"blocked";
        Write(deniedPath/L"private.tmp", "blocked directory payload", old);
        DWORD securitySize = 0;
        GetFileSecurityW(deniedPath.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &securitySize);
        std::vector<BYTE> security(securitySize);
        Check(securitySize != 0 && GetFileSecurityW(deniedPath.c_str(), DACL_SECURITY_INFORMATION,
            security.data(), securitySize, &securitySize), "Synthetic file permissions saved");
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
        Check(GetSecurityDescriptorControl(security.data(), &control, &revision) != 0, "Synthetic DACL inheritance state saved");
        PSECURITY_DESCRIPTOR denied = nullptr;
        Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)",
            SDDL_REVISION_1, &denied, nullptr) != 0, "Synthetic directory enumeration denial descriptor prepared");
        const bool applied = SetFileSecurityW(deniedPath.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, denied) != 0;
        LocalFree(denied);
        Check(applied, "Enumeration access denied only on the synthetic directory");
        const auto restore = [&]() { return SetFileSecurityW(deniedPath.c_str(), DACL_SECURITY_INFORMATION |
            ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION), security.data()) != 0; };
        try { result = Cleanup::Scan(options); }
        catch (...) { restore(); throw; }
        Check(restore(), "Synthetic directory permissions restored");
        Check(result.incomplete && result.failures != 0 && result.coverage[0].failures != 0 &&
            result.coverage[0].state == Cleanup::CoverageState::Partial, "Unreadable candidate directory reports partial failure instead of silently reporting zero");
        std::wstring normalized, error;
        Check(!Cleanup::NormalizeDirectory(L"Temp", normalized, error), "Relative exclusion is rejected");
        Check(!Cleanup::NormalizeDirectory(L"C:\\", normalized, error), "Volume root is rejected");
        Check(!Cleanup::NormalizeDirectory(L"C:\\Temp\\*", normalized, error), "Wildcard directory is rejected");
        std::vector<std::wstring> parsed, decoded;
        const auto unicodeExclusion = (temp/L"排除 空格;é").wstring();
        const auto secondExclusion = (roaming/L"Code/CachedData").wstring();
        Check(Cleanup::ParseExclusions(unicodeExclusion + L"\\\r\n" + secondExclusion + L"\r\n" + unicodeExclusion, parsed, error) && parsed.size() == 2,
            "Exclusions normalize and deduplicate complete Unicode directory lines");
        const auto encoded = Cleanup::EncodeExclusions(parsed);
        Check(std::all_of(encoded.begin(), encoded.end(), [](wchar_t c) { return c < 128; }) &&
            Cleanup::DecodeExclusions(encoded, decoded, error) && decoded == parsed,
            "Dedicated ASCII setting round trips Unicode exclusions without INI code page loss");
        Check(!Cleanup::DecodeExclusions(encoded + L",00G0", decoded, error) && decoded.empty(),
            "A corrupt later entry never returns earlier saved exclusions as a partial success");
        Check(Cleanup::ParseExclusions(L"", parsed, error) && parsed.empty() &&
            Cleanup::DecodeExclusions(Cleanup::EncodeExclusions(parsed), decoded, error) && decoded.empty(),
            "An explicit empty exclusion list survives encoding and decoding");
        for (const auto* invalid : { L"v9:0041", L"v1:00G0", L"v1:004", L"v1:0000", L"v1:0043", L"v1:0043003A005C" }) {
            decoded = { unicodeExclusion };
            Check(!Cleanup::DecodeExclusions(invalid, decoded, error) && decoded.empty(),
                "Malformed, unknown-version and unsafe saved exclusions fail without a partial list");
        }
        Check(!Cleanup::ParseExclusions(unicodeExclusion + L"\r\nrelative", parsed, error) && parsed.empty(),
            "Invalid exclusion drafts do not yield a partially valid replacement");
        Check(Cleanup::IsWithin((temp/L"nested").wstring(), temp.wstring()), "Directory boundary includes descendants");
        Check(!Cleanup::IsWithin((local/L"TempSibling").wstring(), temp.wstring()), "Directory boundary rejects same-prefix sibling");
        restricted = options; restricted.maximumCandidates = 1;
        result = Cleanup::Scan(restricted);
        Check(result.limitReached && result.incomplete && result.candidates.size() == 1, "Candidate limit is an explicitly incomplete preview");
        Check(result.coverage[0].state == Cleanup::CoverageState::Partial && result.coverage[1].state == Cleanup::CoverageState::NotScanned,
            "Limit distinguishes interrupted category from categories not scanned");
        std::stop_source stop;
        result = Cleanup::Scan(options, stop.get_token(), [&](const Cleanup::Progress& progress) {
            if (progress.checkedFiles >= 1) stop.request_stop();
        });
        Check(result.cancelled && result.incomplete, "Cancellation never reports a complete preview");
        Check(result.coverage[0].state == Cleanup::CoverageState::Partial && result.coverage[1].state == Cleanup::CoverageState::NotScanned,
            "Cancellation does not label unscanned cache directories as missing");
        Write(temp/L"stream.tmp", "stream payload", old);
        { std::ofstream stream(temp/L"stream.tmp:user-data", std::ios::binary); stream << "personal stream"; }
        Write(temp/L"hardlink.tmp", "hardlink payload", old);
        Check(CreateHardLinkW((temp/L"hardlink-alias.tmp").c_str(), (temp/L"hardlink.tmp").c_str(), nullptr) != 0, "Synthetic hardlink created");
        result = Cleanup::Scan(options);
        Check(!Has(result, temp/L"stream.tmp"), "A file with named streams is skipped");
        Check(!Has(result, temp/L"hardlink.tmp") && !Has(result, temp/L"hardlink-alias.tmp"), "Hardlinked payloads do not inflate recoverable space");
        const auto outside = root/L"personal";
        Write(outside/L"private.tmp", "outside sentinel", old);
        Junction(temp/L"linked-directory", outside);
        result = Cleanup::Scan(options);
        Check(!Has(result, temp/L"linked-directory/private.tmp"), "Directory junction is never traversed");
        restricted = options; restricted.localAppData = (root/L"linked-anchor").wstring();
        Junction(restricted.localAppData, local);
        result = Cleanup::Scan(restricted);
        Check(!Has(result, temp/L"old.tmp") && Has(result, code/L"compiled.bin") && result.incomplete,
            "An ancestor junction cannot redirect a local anchor or suppress an independent valid roaming anchor");
        Check(result.coverage[0].state == Cleanup::CoverageState::Failed, "Unverifiable anchor reports failure for enabled categories");
        restricted = options; restricted.roamingAppData = (root/L"linked-roaming").wstring();
        Junction(restricted.roamingAppData, roaming);
        result = Cleanup::Scan(restricted);
        Check(result.incomplete && !Has(result, code/L"compiled.bin") && Has(result, temp/L"old.tmp") &&
            result.coverage[static_cast<size_t>(Cleanup::Category::VSCode)].state == Cleanup::CoverageState::Failed,
            "A roaming ancestor junction never redirects cache recognition or disables independent local results");
        Check(Snapshot(outside).size() == 1, "Outside sentinel remains intact");
        RunPlanTests(root, now, old);
        RunPlanGuardTests(root, now, old);
        std::cout << "{\"status\":\"PASS\",\"checks\":" << checks << "}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Cleanup preview test failed: " << error.what() << std::endl;
        return 1;
    }
}
