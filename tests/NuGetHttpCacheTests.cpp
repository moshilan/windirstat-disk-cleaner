// Official NuGet HTTP cleanup tests. Every affected file is created by this process.
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "Cleanup/NuGetHttpCache.h"
namespace fs = std::filesystem;
using namespace Cleanup::NuGetHttp;
static unsigned checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
static void Write(const fs::path& p, const std::string& value = "synthetic sentinel")
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << value;
}
static std::string Read(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
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
    Check(ok, "Own synthetic junction created without elevation");
}
struct Fixture
{
    fs::path root, http;
    std::vector<fs::path> sentinels;
    Fixture(const fs::path& run, const wchar_t* name) : root(run / name), http(root / L"NuGetHttp")
    {
        fs::create_directories(root / L"LocalAppData");
        Write(root / L".windirstat-cleanup-fixture", "WinDirStat read-only cleanup fixture v1");
        const std::pair<const wchar_t*, const wchar_t*> envs[] = {
            {L"NUGET_PACKAGES",L"packages"}, {L"NUGET_SCRATCH",L"scratch"},
            {L"NUGET_PLUGINS_CACHE_PATH",L"plugins"}, {L"DOTNET_CLI_HOME",L"cli-home"},
            {L"TEMP",L"temp"}, {L"TMP",L"temp"} };
        for (const auto& [key, dir] : envs)
        {
            auto path = root / dir;
            fs::create_directories(path);
            SetEnvironmentVariableW(key, path.c_str());
            if (key[0] == L'N') { Write(path / L"sentinel"); sentinels.push_back(path / L"sentinel"); }
        }
        // Fixture resolver uses GetTempPath: keep TEMP at the run's parent, not inside the fixture.
        SetEnvironmentVariableW(L"TEMP", run.parent_path().c_str());
        SetEnvironmentVariableW(L"TMP", run.parent_path().c_str());
        SetEnvironmentVariableW(L"WDS_CLEANUP_TEST_ROOT", root.c_str());
        SetEnvironmentVariableW(L"NUGET_HTTP_CACHE_PATH", http.c_str());
        SetEnvironmentVariableW(L"DOTNET_SKIP_FIRST_TIME_EXPERIENCE", L"1");
        SetEnvironmentVariableW(L"DOTNET_CLI_TELEMETRY_OPTOUT", L"1");
        Write(http / L"source" / L"a.dat", std::string(12345, 'a'));
        Write(http / L"source" / L"b.dat", std::string(6789, 'b'));
        Write(root / L"project" / L"project.csproj"); sentinels.push_back(root / L"project" / L"project.csproj");
    }
    void Preserved() const
    {
        for (const auto& p : sentinels) Check(Read(p) == "synthetic sentinel", "Other caches and project files unchanged");
    }
};
static void TestPip(const fs::path& run, const std::wstring& python, const std::wstring& withoutPip)
{
    const ExclusionPolicy none{ true, L"" };
    const auto setup = [](const Fixture& f)
    {
        const auto cache = f.root / L"PipCache";
        SetEnvironmentVariableW(L"PIP_CACHE_DIR", cache.c_str());
        SetEnvironmentVariableW(L"PIP_CONFIG_FILE", L"NUL");
        SetEnvironmentVariableW(L"PIP_PYTHON", nullptr);
        SetEnvironmentVariableW(L"PIP_LOG", nullptr);
        Write(cache / L"http" / L"old.body", std::string(12345, 'a'));
        Write(cache / L"http-v2" / L"new.body", std::string(6789, 'b'));
        Write(cache / L"wheels" / L"hash" / L"synthetic-1-py3-none-any.whl", std::string(4097, 'c'));
        Write(cache / L"unmanaged-sentinel", "keep");
        return cache;
    };
    {
        Fixture f(run, L"pip-success"); const auto cache = setup(f);
        SetEnvironmentVariableW(L"PIP_PYTHON", withoutPip.c_str());
        SetEnvironmentVariableW(L"PIP_LOG", (f.root / L"project/should-not-be-written.log").c_str());
        auto p = PreparePip(none, python);
        if (p.state != State::Ready) std::wcerr << p.diagnostic << L"\n";
        Check(p.state == State::Ready && p.provider == Provider::Pip && fs::equivalent(p.cachePath, cache), "Selected Python reports its actual isolated pip cache");
        Check(!p.environmentDetails.empty() && p.command.find(L"cache purge") != std::wstring::npos && p.command.find(python) != std::wstring::npos,
            "Pip scope exposes selected interpreter, environment identity and fixed purge command");
        std::stop_source stop; stop.request_stop();
        auto r = Execute(p, none, stop.get_token());
        Check(r.state == State::Cancelled && !r.started && fs::exists(cache / L"http/old.body"), "Cancelled pip request starts no purge");
        auto forged = p; forged.provider = Provider::NuGetHttp;
        Check(!Execute(forged, none).started, "Provider identity cannot be changed after pip scope review");
        Check(!Execute(p, { true, (cache / L"wheels").wstring() }).started, "An excluded wheel subtree blocks the entire pip purge");
        r = Execute(p, none);
        if (r.state != State::Success) std::wcerr << r.diagnostic << L"\n" << r.output << L"\n";
        Check(r.state == State::Success && r.started && r.exitCode == 0, "Official pip purge succeeds on synthetic cache only");
        Check(!fs::exists(f.root / L"project/should-not-be-written.log"), "Pinned pip logging never writes a configured project log");
        Check(!fs::exists(cache / L"http/old.body") && !fs::exists(cache / L"http-v2/new.body") &&
            !fs::exists(cache / L"wheels/hash/synthetic-1-py3-none-any.whl"), "Official purge clears legacy HTTP, current HTTP and wheel contents");
        Check(Read(cache / L"unmanaged-sentinel") == "keep" && r.before.logicalBytes && *r.before.logicalBytes == 23235 &&
            r.after.logicalBytes && *r.after.logicalBytes == 4, "Pip result measures retained unmanaged files instead of assuming an empty directory");
        Check(Read(f.http / L"source/a.dat") == std::string(12345, 'a'), "NuGet HTTP cache survives pip purge"); f.Preserved();
        std::cout << "{\"case\":\"pip_success_cancel\",\"before\":23235,\"after\":4,\"exit\":0}\n";
    }
    {
        Fixture f(run, L"pip-locked"); const auto cache = setup(f); auto p = PreparePip(none, python);
        Check(p.state == State::Ready, "Locked pip scope prepared");
        auto h = CreateFileW((cache / L"http-v2/new.body").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(h != INVALID_HANDLE_VALUE, "Own pip HTTP payload locked");
        auto r = Execute(p, none); CloseHandle(h);
        Check(r.state == State::Failed && r.started && r.exitCode != 0, "Pip locked-file failure never falls back to other cleanup");
        Check(Read(cache / L"http-v2/new.body") == std::string(6789, 'b') && Read(cache / L"unmanaged-sentinel") == "keep", "Locked and unmanaged pip contents remain"); f.Preserved();
    }
    {
        Fixture f(run, L"pip-guards"); const auto cache = setup(f);
        auto blocked = PreparePip({ true, (cache / L"wheels").wstring() }, python);
        Check(blocked.state == State::Blocked && blocked.exclusionFailure == ExclusionFailure::Overlap && !blocked.prepared, "Pip preparation enforces exclusions");
        blocked = PreparePip({}, python);
        Check(blocked.state == State::Blocked && !blocked.prepared, "Unknown pip exclusions fail closed");
        auto p = PreparePip(none, python); Check(p.state == State::Ready, "Pip scope prepared before environment change");
        SetEnvironmentVariableW(L"PIP_CACHE_DIR", (f.root / L"project").c_str());
        Check(!Execute(p, none).started, "Changing pip cache environment after review blocks execution");
        SetEnvironmentVariableW(L"PIP_CACHE_DIR", cache.c_str());
        const auto config = f.root / L"pip-test.ini";
        Write(config, "[global]\ntimeout = 15\nindex-url = https://synthetic-user:synthetic-password@example.invalid/simple\n");
        SetEnvironmentVariableW(L"PIP_CONFIG_FILE", config.c_str());
        p = PreparePip(none, python); Check(p.state == State::Ready, "Selected pip configuration fingerprint captured");
        Check((p.diagnostic + p.environmentDetails + p.command).find(L"synthetic-password") == std::wstring::npos,
            "Configuration credentials stay out of displayed pip scope and diagnostics");
        Write(config, "[global]\ntimeout = 16\nindex-url = https://synthetic-user:synthetic-password@example.invalid/simple\n");
        Check(!Execute(p, none).started && fs::exists(cache / L"http/old.body"), "Pip config file changes block purge even when directory is unchanged");
        SetEnvironmentVariableW(L"PIP_CONFIG_FILE", L"NUL");
        auto missing = PreparePip(none, withoutPip);
        Check(missing.state == State::Unavailable && !missing.prepared && !Execute(missing, none).started,
            "A selected environment without pip never falls back to another Python");
        p = PreparePip(none, python); Check(p.state == State::Ready, "Pip scope prepared before directory replacement");
        fs::rename(cache, f.root / L"retained-pip"); Write(cache / L"replacement", "replacement");
        Check(!Execute(p, none).started && Read(cache / L"replacement") == "replacement", "Replaced pip cache identity blocks official purge"); f.Preserved();
    }
    {
        Fixture f(run, L"pip-junction"); const auto cache = setup(f);
        Junction(cache / L"linked", f.root / L"project");
        auto p = PreparePip(none, python);
        Check(p.state == State::Blocked && !Execute(p, none).started, "A pip cache junction fails closed"); f.Preserved();
    }
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        wchar_t temp[32768]{}; Check(GetTempPathW(32768, temp) != 0, "Temporary root available");
        const ExclusionPolicy noExclusions{ true, L"" };
        const auto run = fs::path(temp) / (L"WinDirStatTests-NuGet-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        {
            Fixture f(run, L"success"); auto p = Prepare(noExclusions);
            if (p.state != State::Ready) std::wcerr << p.diagnostic << L"\n";
            Check(p.state == State::Ready && p.prepared && fs::equivalent(p.cachePath, f.http), "Official command resolves isolated actual scope");
            Check(p.command.find(L"http-cache") != std::wstring::npos && p.command.find(L" all ") == std::wstring::npos, "Only HTTP-cache command allowed");
            std::stop_source cancelled; cancelled.request_stop();
            auto c = Execute(p, noExclusions, cancelled.get_token());
            Check(c.state == State::Cancelled && !c.started && fs::exists(f.http / L"source/a.dat"), "Pre-start cancellation performs no cleanup");
            auto forged = p; forged.cachePath = (f.root / L"project").wstring();
            Check(!Execute(forged, noExclusions).started, "Modified displayed scope cannot execute");
            auto r = Execute(p, noExclusions);
            if (r.state != State::Success) std::wcerr << r.diagnostic << L"\n" << r.output << L"\n";
            Check(r.started && r.state == State::Success && r.exitCode == 0, "Actual official HTTP-cache cleanup succeeds");
            Check(!fs::exists(f.http / L"source/a.dat") && !fs::exists(f.http / L"source/b.dat"), "Whole synthetic HTTP cache cleared");
            Check(r.before.logicalBytes && *r.before.logicalBytes == 19134 && r.after.logicalBytes && *r.after.logicalBytes == 0,
                "Logical byte observations reflect actual contents, not exit code");
            f.Preserved();
            std::cout << "{\"case\":\"success_and_cancel\",\"cache_before\":19134,\"cache_after\":0,\"exit\":0}\n";
        }
        {
            Fixture f(run, L"locked"); auto p = Prepare(noExclusions); Check(p.state == State::Ready, "Locked case prepared");
            auto h = CreateFileW((f.http / L"source/a.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            Check(h != INVALID_HANDLE_VALUE, "Own file locked without delete sharing");
            auto r = Execute(p, noExclusions); CloseHandle(h);
            Check(r.state == State::Failed && r.started && r.exitCode != 0, "Official locked-file failure retained");
            Check(Read(f.http / L"source/a.dat") == std::string(12345, 'a'), "Locked file unchanged");
            Check(r.after.logicalBytes && *r.after.logicalBytes >= 12345, "Failure measures surviving content"); f.Preserved();
            std::cout << "{\"case\":\"locked_partial_failure\",\"exit\":" << r.exitCode << "}\n";
        }
        {
            Fixture f(run, L"replace"); auto p = Prepare(noExclusions); Check(p.state == State::Ready, "Replacement case prepared");
            fs::rename(f.http, f.root / L"original"); Write(f.http / L"replacement", "replacement");
            Check(!Execute(p, noExclusions).started && Read(f.http / L"replacement") == "replacement", "Replaced root blocked before official cleanup"); f.Preserved();
        }
        {
            Fixture f(run, L"junction"); Junction(f.http / L"redirect", f.root / L"project");
            auto p = Prepare(noExclusions); Check(p.state == State::Blocked && !Execute(p, noExclusions).started, "Descendant junction blocks cleanup"); f.Preserved();
        }
        {
            Fixture f(run, L"hardlink");
            Check(CreateHardLinkW((f.http / L"linked").c_str(), (f.root / L"project/project.csproj").c_str(), nullptr), "Own hard link created");
            auto p = Prepare(noExclusions); Check(p.state == State::Blocked && !Execute(p, noExclusions).started, "Hard link blocks cleanup"); f.Preserved();
        }
        {
            Fixture f(run, L"override"); SetEnvironmentVariableW(L"NUGET_HTTP_CACHE_PATH", (f.root / L"project").c_str());
            auto p = Prepare(noExclusions); Check(p.state == State::Blocked && !Execute(p, noExclusions).started, "Unsafe actual override never falls back to standard cache"); f.Preserved();
        }
        {
            Fixture f(run, L"cache-alias");
            fs::rename(f.root / L"packages", f.root / L"saved-packages");
            Junction(f.root / L"packages", f.http);
            auto p = Prepare(noExclusions); Check(p.state == State::Blocked && !Execute(p, noExclusions).started, "Other cache junction aliasing HTTP blocks cleanup");
            Check(Read(f.root / L"saved-packages/sentinel") == "synthetic sentinel" && fs::exists(f.http / L"source/a.dat"),
                "Aliased caches and preserved packages remain untouched");
        }
        {
            Fixture f(run, L"exclusion-conflicts");
            auto slashCase = f.http.wstring();
            for (auto& c : slashCase) { if (c == L'\\') c = L'/'; else c = static_cast<wchar_t>(towupper(c)); }
            const std::vector<std::wstring> conflicts{ f.http.wstring(), f.root.wstring(),
                (f.http / L"source").wstring(), (f.http / L"not-created").wstring(), slashCase + L"/" };
            for (const auto& directory : conflicts)
            {
                const ExclusionPolicy policy{ true, directory };
                auto p = Prepare(policy);
                Check(p.state == State::Blocked && p.exclusionFailure == ExclusionFailure::Overlap && !p.prepared,
                    "Self, ancestor, child, missing child and normalized aliases block the whole official scope");
                Check(!Execute(p, policy).started && fs::exists(f.http / L"source/a.dat"), "A blocked exclusion scope cannot be executed");
            }
            auto p = Prepare(noExclusions); Check(p.state == State::Ready, "Scope prepared before exclusion change");
            auto r = Execute(p, { true, (f.http / L"source").wstring() });
            Check(r.state == State::Blocked && r.exclusionFailure == ExclusionFailure::Overlap && !r.started,
                "An exclusion introduced after preview blocks execution despite earlier confirmation");
            r = Execute(p, { false, L"" });
            Check(r.state == State::Blocked && r.exclusionFailure == ExclusionFailure::Unavailable && !r.started,
                "Unavailable rules at execution cannot fall back to empty rules");
            for (const auto& policy : { ExclusionPolicy{}, ExclusionPolicy{ true, L"relative-directory" } })
            {
                auto failed = Prepare(policy);
                Check(failed.state == State::Blocked && failed.exclusionFailure == ExclusionFailure::Unavailable && !Execute(failed, policy).started,
                    "Missing or invalid rules fail closed before cleanup");
            }
            wchar_t shortPath[32768]{};
            Check(GetShortPathNameW(f.http.c_str(), shortPath, 32768) != 0, "Own cache short-path spelling obtained");
            auto alias = Prepare({ true, std::wstring(shortPath) + L"\\missing-child" });
            Check(alias.state == State::Blocked && alias.exclusionFailure == ExclusionFailure::Overlap,
                "Missing excluded child under an existing short-path ancestor remains protected");
            Junction(f.root / L"excluded-alias", f.http);
            auto unsafe = Prepare({ true, (f.root / L"excluded-alias").wstring() });
            Check(unsafe.state == State::Blocked && unsafe.exclusionFailure == ExclusionFailure::Unavailable && !unsafe.prepared,
                "An excluded junction whose scope cannot be reliably checked blocks cleanup");
            Check(Read(f.http / L"source/a.dat") == std::string(12345, 'a'), "Every blocked case preserves the entire HTTP payload");
            f.Preserved();
        }
        {
            Fixture f(run, L"exclusion-neighbor");
            const ExclusionPolicy policy{ true, f.http.wstring() + L"-sibling" };
            auto p = Prepare(policy); Check(p.state == State::Ready, "Similarly named excluded sibling does not overlap HTTP cache");
            auto changed = Execute(p, { true, (f.root / L"project").wstring() });
            Check(changed.state == State::Blocked && changed.exclusionFailure == ExclusionFailure::Changed && !changed.started,
                "A different non-overlapping policy also requires fresh scope review");
            auto r = Execute(p, policy);
            Check(r.state == State::Success && r.started && r.after.logicalBytes && *r.after.logicalBytes == 0,
                "Non-overlapping exclusions still allow normal confirmed official cleanup");
            f.Preserved();
        }
        if (argc == 3) TestPip(run, argv[1], argv[2]);
        else Check(argc == 1, "Pip tests require explicit normal and no-pip interpreter paths");
        std::cout << "{\"status\":\"PASS\",\"checks\":" << checks << ",\"fixtures_retained\":true}\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
