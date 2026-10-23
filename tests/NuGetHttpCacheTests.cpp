// Official NuGet HTTP cleanup tests. Every affected file is created by this process.
#include <windows.h>
#include <winioctl.h>
#include <cstring>
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
int wmain()
{
    try
    {
        wchar_t temp[32768]{}; Check(GetTempPathW(32768, temp) != 0, "Temporary root available");
        const auto run = fs::path(temp) / (L"WinDirStatTests-NuGet-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        {
            Fixture f(run, L"success"); auto p = Prepare();
            if (p.state != State::Ready) std::wcerr << p.diagnostic << L"\n";
            Check(p.state == State::Ready && p.prepared && fs::equivalent(p.cachePath, f.http), "Official command resolves isolated actual scope");
            Check(p.command.find(L"http-cache") != std::wstring::npos && p.command.find(L" all ") == std::wstring::npos, "Only HTTP-cache command allowed");
            std::stop_source cancelled; cancelled.request_stop();
            auto c = Execute(p, cancelled.get_token());
            Check(c.state == State::Cancelled && !c.started && fs::exists(f.http / L"source/a.dat"), "Pre-start cancellation performs no cleanup");
            auto forged = p; forged.cachePath = (f.root / L"project").wstring();
            Check(!Execute(forged).started, "Modified displayed scope cannot execute");
            auto r = Execute(p);
            if (r.state != State::Success) std::wcerr << r.diagnostic << L"\n" << r.output << L"\n";
            Check(r.started && r.state == State::Success && r.exitCode == 0, "Actual official HTTP-cache cleanup succeeds");
            Check(!fs::exists(f.http / L"source/a.dat") && !fs::exists(f.http / L"source/b.dat"), "Whole synthetic HTTP cache cleared");
            Check(r.before.logicalBytes && *r.before.logicalBytes == 19134 && r.after.logicalBytes && *r.after.logicalBytes == 0,
                "Logical byte observations reflect actual contents, not exit code");
            f.Preserved();
            std::cout << "{\"case\":\"success_and_cancel\",\"cache_before\":19134,\"cache_after\":0,\"exit\":0}\n";
        }
        {
            Fixture f(run, L"locked"); auto p = Prepare(); Check(p.state == State::Ready, "Locked case prepared");
            auto h = CreateFileW((f.http / L"source/a.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            Check(h != INVALID_HANDLE_VALUE, "Own file locked without delete sharing");
            auto r = Execute(p); CloseHandle(h);
            Check(r.state == State::Failed && r.started && r.exitCode != 0, "Official locked-file failure retained");
            Check(Read(f.http / L"source/a.dat") == std::string(12345, 'a'), "Locked file unchanged");
            Check(r.after.logicalBytes && *r.after.logicalBytes >= 12345, "Failure measures surviving content"); f.Preserved();
            std::cout << "{\"case\":\"locked_partial_failure\",\"exit\":" << r.exitCode << "}\n";
        }
        {
            Fixture f(run, L"replace"); auto p = Prepare(); Check(p.state == State::Ready, "Replacement case prepared");
            fs::rename(f.http, f.root / L"original"); Write(f.http / L"replacement", "replacement");
            Check(!Execute(p).started && Read(f.http / L"replacement") == "replacement", "Replaced root blocked before official cleanup"); f.Preserved();
        }
        {
            Fixture f(run, L"junction"); Junction(f.http / L"redirect", f.root / L"project");
            auto p = Prepare(); Check(p.state == State::Blocked && !Execute(p).started, "Descendant junction blocks cleanup"); f.Preserved();
        }
        {
            Fixture f(run, L"hardlink");
            Check(CreateHardLinkW((f.http / L"linked").c_str(), (f.root / L"project/project.csproj").c_str(), nullptr), "Own hard link created");
            auto p = Prepare(); Check(p.state == State::Blocked && !Execute(p).started, "Hard link blocks cleanup"); f.Preserved();
        }
        {
            Fixture f(run, L"override"); SetEnvironmentVariableW(L"NUGET_HTTP_CACHE_PATH", (f.root / L"project").c_str());
            auto p = Prepare(); Check(p.state == State::Blocked && !Execute(p).started, "Unsafe actual override never falls back to standard cache"); f.Preserved();
        }
        {
            Fixture f(run, L"cache-alias");
            fs::rename(f.root / L"packages", f.root / L"saved-packages");
            Junction(f.root / L"packages", f.http);
            auto p = Prepare(); Check(p.state == State::Blocked && !Execute(p).started, "Other cache junction aliasing HTTP blocks cleanup");
            Check(Read(f.root / L"saved-packages/sentinel") == "synthetic sentinel" && fs::exists(f.http / L"source/a.dat"),
                "Aliased caches and preserved packages remain untouched");
        }
        std::cout << "{\"status\":\"PASS\",\"checks\":" << checks << ",\"fixtures_retained\":true}\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
