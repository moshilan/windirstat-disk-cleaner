// Real BleachBit CLI tests. Both TEMP roots contain only files created here;
// Windows TEMP is protected and only user TEMP is cleaned. Fixtures are kept.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "Cleanup/BleachBit.h"
#include "Cleanup/CleanupPreview.h"

namespace fs = std::filesystem;
namespace BB = Cleanup::BleachBit;
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
static unsigned checks = 0, cases = 0;
static void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
static std::string Read(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Read own sentinel");
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}
static void Write(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) throw std::runtime_error("Write own fixture");
}
struct EnvironmentGuard
{
    std::wstring name, original;
    bool existed = false;
    explicit EnvironmentGuard(std::wstring key) : name(std::move(key))
    {
        const DWORD size = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
        existed = size != 0;
        if (size)
        {
            original.resize(size);
            const DWORD copied = GetEnvironmentVariableW(name.c_str(), original.data(), size);
            Check(copied && copied < size, "Read original environment value");
            original.resize(copied);
        }
    }
    ~EnvironmentGuard() { SetEnvironmentVariableW(name.c_str(), existed ? original.c_str() : nullptr); }
};
static bool OrdinaryToken()
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    Handle token(raw, CloseHandle);
    TOKEN_ELEVATION elevation{}; DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) || elevation.TokenIsElevated) return false;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &administrators)) return false;
    GetTokenInformation(token.get(), TokenGroups, nullptr, 0, &size);
    std::vector<unsigned char> storage(size);
    const bool read = size && GetTokenInformation(token.get(), TokenGroups, storage.data(), size, &size);
    bool ordinary = read;
    if (read)
    {
        const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(storage.data());
        for (DWORD i = 0; i < groups->GroupCount; ++i)
            if (EqualSid(groups->Groups[i].Sid, administrators) &&
                (groups->Groups[i].Attributes & SE_GROUP_ENABLED) && !(groups->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY)) ordinary = false;
    }
    FreeSid(administrators);
    return ordinary;
}
struct Fixture
{
    EnvironmentGuard environment{ L"WDS_CLEANUP_TEST_ROOT" };
    fs::path root, user, system;
    std::vector<fs::path> payloads;
    std::map<fs::path, std::string> outside;
    Fixture()
    {
        wchar_t temporary[32768]{}, id[40]{}; GUID guid{};
        const DWORD size = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
        Check(size && size < std::size(temporary) && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, id, 40), "Generate private synthetic root");
        std::wstring parent, error;
        Check(Cleanup::NormalizeDirectory(temporary, parent, error), "Normalize synthetic root parent");
        root = fs::path(parent) / (L"WinDirStatTests-BleachBit-" + std::wstring(id)) / L"fixture";
        Check(!fs::exists(root), "Refuse an existing fixture");
        user = root / L"LocalAppData" / L"Temp";
        system = root / L"Windows" / L"Temp";
        fs::create_directories(user); fs::create_directories(system); fs::create_directories(root / L"Control");
        Write(root / L".windirstat-cleanup-fixture", "WinDirStat read-only cleanup fixture v1\n");
        payloads = { user / L"recent-arbitrary.data", user / L"nested/recent-second.bin" };
        for (size_t i = 0; i < payloads.size(); ++i) Write(payloads[i], std::string(4097 + i * 97, static_cast<char>('A' + i)));
        Write(system / L"protected-system-temp.bin", "synthetic Windows Temp must remain protected");
        outside.emplace(system / L"protected-system-temp.bin", "synthetic Windows Temp must remain protected");
        for (const auto& relative : { L"Project/project.txt", L"OtherCache/payload.bin", L"LocalAppData/pip/Cache/http-v2/keep.body",
            L"LocalAppData/NuGet/v3-cache/keep.dat", L"LocalAppData/npm-cache/_npx/keep.bin", L"Personal/sentinel.txt" })
        {
            const auto path = root / relative;
            const auto contents = std::string("synthetic outside scope sentinel ") + std::to_string(outside.size());
            Write(path, contents); outside.emplace(path, contents);
        }
        Check(SetEnvironmentVariableW(L"WDS_CLEANUP_TEST_ROOT", root.c_str()) != FALSE, "Select only own marked fixture");
    }
    void OutsideUnchanged() const
    {
        for (const auto& [path, contents] : outside) Check(fs::exists(path) && Read(path) == contents, "Outside category sentinel remains byte-identical");
    }
    void PayloadsPresent() const
    {
        for (const auto& path : payloads) Check(fs::exists(path) && !Read(path).empty(), "Preview or refusal preserves each own source");
    }
    void CheckScope(const BB::Preview& preview) const
    {
        Check(_wcsicmp(preview.userTemp.c_str(), user.c_str()) == 0, "Actual TEMP is the complete self-created user root");
        Check(_wcsicmp(preview.systemTemp.c_str(), system.c_str()) == 0, "Actual Windows TEMP is the complete self-created system root");
    }
};
static BB::Preview Prepare(Fixture& fixture, const std::wstring& tool)
{
    auto preview = BB::Prepare({ true, L"" }, tool);
    if (preview.state != BB::State::Ready) std::wcerr << L"Prepare diagnostic: " << preview.diagnostic << L"\n" << preview.output << L"\n";
    Check(preview.state == BB::State::Ready && preview.prepared, "Real supported BleachBit preview is ready");
    fixture.CheckScope(preview);
    Check(preview.version.find(L"6.0.6") != std::wstring::npos, "Only the reviewed engine version is accepted");
    Check(preview.command.find(L"system.tmp") != std::wstring::npos && preview.command.find(L"--clean") != std::wstring::npos,
        "Prepared scope identifies the sole allowed cleanup command");
    return preview;
}
static void Print(const char* name, const BB::Result& result)
{
    ++cases;
    std::cout << "{\"case\":\"" << name << "\",\"state\":" << static_cast<unsigned>(result.state)
        << ",\"started\":" << (result.started ? "true" : "false") << ",\"exit_code\":" << result.exitCode;
    if (result.files) std::cout << ",\"engine_files\":" << *result.files;
    if (result.errors) std::cout << ",\"engine_errors\":" << *result.errors;
    if (result.remainingFiles) std::cout << ",\"remaining_files\":" << *result.remainingFiles;
    std::cout << "}\n";
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        Check(argc == 2, "Pass exactly one reviewed external engine path");
        const std::wstring tool = fs::path(argv[1]).wstring();
        Check(fs::path(tool).is_absolute() && fs::is_regular_file(tool), "Existing absolute external engine path required");
        Check(OrdinaryToken(), "Tests and real engine must run without elevation or enabled Administrators membership");
        {
            Fixture fixture;
            const auto preview = Prepare(fixture, tool);
            Check(preview.files && *preview.files >= fixture.payloads.size(), "Engine preview includes recent non-tmp extensions within the user root");
            Check(preview.output.find(L"recent-arbitrary.data") != std::wstring::npos && preview.output.find(L"recent-second.bin") != std::wstring::npos,
                "Actual engine preview names both synthetic payloads");
            fixture.PayloadsPresent(); fixture.OutsideUnchanged();
            const auto result = BB::Execute(preview, { true, L"" }); Print("real_clean", result);
            if (result.state != BB::State::Success) std::wcerr << L"Clean diagnostic: " << result.diagnostic << L"\n" << result.output << L"\n" << result.postPreview << L"\n";
            Check(result.state == BB::State::Success && result.started && result.exitCode == 0, "Real CLI cleaning completes successfully");
            Check(result.files && *result.files >= fixture.payloads.size() && result.errors.value_or(0) == 0, "Engine reports the cleaned items without errors");
            Check(result.remainingFiles && *result.remainingFiles == 0, "Independent engine re-preview finds no remaining category files");
            for (const auto& path : fixture.payloads) Check(!fs::exists(path), "Actual category payload was removed by BleachBit");
            fixture.OutsideUnchanged();
        }
        for (unsigned variant = 0; variant < 4; ++variant)
        {
            Fixture fixture;
            const std::wstring excluded = (variant == 0 ? fixture.user : variant == 1 ? fixture.user.parent_path() :
                variant == 2 ? fixture.user / L"nested" : fixture.user / L"not-created/child").wstring();
            const BB::ExclusionPolicy policy{ true, excluded };
            const auto preview = BB::Prepare(policy, tool);
            Check(preview.state == BB::State::Blocked, "Any existing or future exclusion overlap blocks the complete category");
            const auto result = BB::Execute(preview, policy); Print("excluded_scope", result);
            Check(!result.started && result.state != BB::State::Success, "A blocked exclusion cannot be overridden by direct Execute");
            fixture.PayloadsPresent(); fixture.OutsideUnchanged();
        }
        {
            Fixture fixture;
            const auto preview = Prepare(fixture, tool);
            auto changed = BB::Execute(preview, { true, (fixture.root / L"Personal").wstring() }); Print("exclusions_changed", changed);
            Check(changed.state == BB::State::Blocked && !changed.started, "Changed non-overlapping exclusion policy invalidates confirmation");
            fixture.PayloadsPresent(); fixture.OutsideUnchanged();
        }
        {
            Fixture fixture;
            const auto unavailable = BB::Prepare({ false, L"" }, tool);
            Check(unavailable.state == BB::State::Blocked, "Unavailable exclusion configuration is never treated as empty");
            const auto invalid = BB::Prepare({ true, L"relative\\uncertain" }, tool);
            Check(invalid.state == BB::State::Blocked, "Invalid exclusion path blocks the category");
            const auto nearby = BB::Prepare({ true, (fixture.root / L"Personal").wstring() }, tool);
            Check(nearby.state == BB::State::Ready, "A reliably separate exclusion does not disable the complete feature");
            const auto protectedSystem = BB::Prepare({ true, fixture.system.wstring() }, tool);
            Check(protectedSystem.state == BB::State::Ready, "Already-protected system Temp exclusion does not widen or disable user-only scope");
            fixture.PayloadsPresent(); fixture.OutsideUnchanged();
        }
        {
            Fixture fixture;
            std::stop_source stop; stop.request_stop();
            const auto cancelled = BB::Prepare({ true, L"" }, tool, stop.get_token());
            Check(cancelled.state == BB::State::Cancelled, "Cancelled discovery cannot become executable");
            const auto preview = Prepare(fixture, tool);
            const auto result = BB::Execute(preview, { true, L"" }, stop.get_token()); Print("cancel_before_clean", result);
            Check(result.state == BB::State::Cancelled && !result.started, "Cancellation before cleanup starts no cleanup process");
            fixture.PayloadsPresent(); fixture.OutsideUnchanged();
        }
        {
            Fixture fixture;
            const auto missing = BB::Prepare({ true, L"" }, (fixture.root / L"missing/bleachbit_console.exe").wstring());
            Check(missing.state != BB::State::Ready && !missing.prepared && !missing.diagnostic.empty(),
                "Missing explicit engine never installs or selects a different environment");
            Check(SetEnvironmentVariableW(L"WDS_CLEANUP_TEST_ROOT", (fixture.root / L"invalid").c_str()) != FALSE, "Select invalid synthetic override");
            const auto invalid = BB::Prepare({ true, L"" }, tool);
            Check(invalid.state != BB::State::Ready && !invalid.prepared, "Invalid fixture never falls back to real Windows directories");
            fixture.PayloadsPresent(); fixture.OutsideUnchanged();
        }
        {
            Fixture fixture;
            const auto preview = Prepare(fixture, tool);
            const auto retained = fixture.root / L"RetainedTemp";
            const bool moved = MoveFileExW(fixture.user.c_str(), retained.c_str(), 0) != FALSE;
            if (moved)
            {
                fs::create_directories(fixture.user);
                Write(fixture.user / L"replacement.data", "replacement must stay");
            }
            else
            {
                Check(GetLastError() == ERROR_SHARING_VIOLATION, "A held scope guard rejects root replacement without relaxing sharing");
                Check(SetEnvironmentVariableW(L"WDS_CLEANUP_TEST_ROOT", (fixture.root / L"changed").c_str()) != FALSE, "Change the active fixture after guarded preview");
            }
            const auto result = BB::Execute(preview, { true, L"" }); Print("scope_changed_or_guarded", result);
            Check(result.state == BB::State::Blocked && !result.started, "Changed root identity or active scope configuration invalidates confirmation");
            if (moved)
                Check(fs::exists(retained / L"recent-arbitrary.data") && Read(fixture.user / L"replacement.data") == "replacement must stay",
                    "Both retained original and replacement root remain untouched");
            else fixture.PayloadsPresent();
            fixture.OutsideUnchanged();
        }
        {
            Fixture fixture;
            const auto preview = Prepare(fixture, tool);
            Handle locked(CreateFileW(fixture.payloads[0].c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr), CloseHandle);
            Check(locked.get() != INVALID_HANDLE_VALUE, "Hold one own payload with exclusive sharing");
            const auto result = BB::Execute(preview, { true, L"" }); Print("locked_partial_failure", result);
            Check(result.started && result.state == BB::State::Failed, "Engine item errors cannot be relabelled success by exit zero");
            Check((result.errors && *result.errors > 0) || result.output.find(L"Errors: ") != std::wstring::npos,
                "Locked-file error remains visible even when the engine summary is incomplete");
            locked.reset();
            Check(fs::exists(fixture.payloads[0]) && !Read(fixture.payloads[0]).empty(), "Locked item remains after failed cleanup");
            Check(!fs::exists(fixture.payloads[1]), "Other in-scope item was actually cleaned, demonstrating partial failure");
            fixture.OutsideUnchanged();
        }
        {
            Fixture fixture;
            // The trigger is observed deletion, not a guessed process startup delay.
            // This is one bounded corpus; no cancellation stress loop or old API experiment.
            for (unsigned i = 0; i < 1000; ++i)
                Write(fixture.user / (L"cancel-" + std::to_wstring(i) + L".data"), "own cancellation fixture");
            const auto preview = Prepare(fixture, tool);
            std::stop_source stop;
            std::atomic_bool observedDeletion = false;
            std::jthread observer([&](std::stop_token done)
            {
                const auto started = GetTickCount64();
                while (!done.stop_requested() && GetTickCount64() - started < 20000)
                {
                    std::error_code error;
                    const bool present = fs::exists(fixture.payloads[0], error);
                    if (!error && !present)
                    {
                        observedDeletion = true;
                        stop.request_stop();
                        return;
                    }
                    Sleep(1);
                }
            });
            const auto result = BB::Execute(preview, { true, L"" }, stop.get_token());
            observer.request_stop(); observer.join(); Print("cancel_after_observed_deletion", result);
            Check(observedDeletion && result.started && result.state == BB::State::Cancelled,
                "Cancellation after a real deletion remains cancelled rather than claiming complete success");
            Check(!fs::exists(fixture.payloads[0]), "Already completed deletion is not falsely reported as rolled back");
            fixture.OutsideUnchanged();
        }
        std::cout << "{\"status\":\"PASS\",\"checks\":" << checks << ",\"cases\":" << cases
            << ",\"real_bleachbit_clean\":true,\"synthetic_only\":true,\"elevated\":false,\"fixtures_retained\":true,\"runtime_cancel_tested\":true}\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "BleachBit synthetic regression failed: " << error.what() << "\n";
        return 1;
    }
}
