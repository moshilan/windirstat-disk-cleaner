// Synthetic installed-application metadata only. Never enumerate a real Uninstall key or MSIX inventory.
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <sddl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "Space/InstalledApps.h"
#include "Space/InstallDirectory.h"
namespace IA = Space::InstalledApps;
static unsigned checks = 0;
static void Check(const bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
struct Key
{
    HKEY value = nullptr;
    ~Key() { if (value) RegCloseKey(value); }
};
static void String(HKEY key, const wchar_t* name, const std::wstring& value)
{
    Check(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS, "Write synthetic string");
}
static void Number(HKEY key, const wchar_t* name, DWORD value)
{
    Check(RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS,
        "Write synthetic DWORD");
}
static void App(const std::wstring& root, const std::wstring& source, const std::wstring& id,
    const std::wstring& name, const DWORD size, bool malformed = false)
{
    Key key;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, (root + L"\\" + source + L"\\" + id).c_str(), 0, nullptr, 0,
        KEY_READ | KEY_WRITE, nullptr, &key.value, nullptr) == ERROR_SUCCESS, "Create owned synthetic application key");
    String(key.value, L"DisplayName", name);
    String(key.value, L"Publisher", L"Synthetic Publisher");
    String(key.value, L"DisplayVersion", L"1.2.3");
    String(key.value, L"InstallLocation", L"C:\\SyntheticOnly\\Application");
    // A command sentinel must never be read as metadata or executed.
    String(key.value, L"UninstallString", L"NEVER_EXECUTE_SYNTHETIC_UNINSTALL_SENTINEL /quiet");
    String(key.value, L"QuietUninstallString", L"NEVER_EXECUTE_SYNTHETIC_UNINSTALL_SENTINEL /silent");
    if (malformed) String(key.value, L"EstimatedSize", L"999999");
    else Number(key.value, L"EstimatedSize", size);
}
namespace fs = std::filesystem;
static fs::path DirectoryFixture()
{
    wchar_t temp[MAX_PATH]{}, text[40]{};
    const auto tempLength = GetTempPathW(MAX_PATH, temp);
    Check(tempLength > 0 && tempLength < MAX_PATH, "Resolve a complete synthetic test location");
    GUID id{};
    Check(SUCCEEDED(CoCreateGuid(&id)), "Generate folder fixture identity");
    StringFromGUID2(id, text, 40);
    const auto root = fs::path(temp) / (L"WinDirStatInstalledAppsShell-" + std::wstring(text + 1, 36) + L" & space[1]");
    Check(root.is_absolute() && !fs::exists(root), "Refuse a relative or reused directory fixture");
    fs::create_directories(root / L"Copilot" / L"Application");
    fs::create_directories(root / L"Edge" / L"Application");
    return root;
}
static void DirectoryRequests()
{
    const auto root = DirectoryFixture();
    const auto target = root / L"Copilot" / L"Application";
    unsigned calls = 0;
    const Space::InstallDirectory::Launch capture = [&](SHELLEXECUTEINFOW* request) -> BOOL
    {
        ++calls;
        Check(request->cbSize == sizeof(*request), "Shell structure is complete");
        Check((request->fMask & SEE_MASK_IDLIST) != 0 && (request->fMask & SEE_MASK_FLAG_NO_UI) != 0,
            "Shell uses explicit folder identity without error popups");
        Check(std::wstring(request->lpVerb) == L"explore", "Explore enters the target instead of selecting it in a parent");
        Check(!request->lpFile && !request->lpParameters && !request->lpDirectory, "No executable, arguments or fallback directory");
        wchar_t actual[MAX_PATH]{};
        Check(SHGetPathFromIDListW(static_cast<PCIDLIST_ABSOLUTE>(request->lpIDList), actual) != FALSE,
            "Shell PIDL resolves to a filesystem target");
        Check(_wcsicmp(actual, target.c_str()) == 0, "Shell receives Copilot Application itself, not Edge or its parent");
        return TRUE;
    };
    Check(SUCCEEDED(Space::InstallDirectory::Open(target.wstring(), nullptr, capture)), "Accept the synthetic installation directory");
    Check(calls == 1, "Exactly one open request, without fallback");
    const auto sentinel = root / L"not-a-directory.exe";
    std::ofstream(sentinel) << "Synthetic data only; never execute";
    const std::vector<std::wstring> invalid{ L"", L"relative", L"C:\\", L"ms-settings:appsfeatures", L"\\\\server\\share",
        (root / L"missing").wstring(), sentinel.wstring(), target.wstring() + L"\\..", target.wstring() + L"\n" };
    for (const auto& path : invalid)
    {
        Check(FAILED(Space::InstallDirectory::Open(path, nullptr, capture)), "Invalid, missing or file target is refused");
        Check(calls == 1, "Rejected path never launches another directory or application");
    }
    unsigned failedLaunches = 0;
    const auto failure = Space::InstallDirectory::Open(target.wstring(), nullptr, [&](SHELLEXECUTEINFOW*) -> BOOL
    { ++failedLaunches; SetLastError(ERROR_ACCESS_DENIED); return FALSE; });
    Check(FAILED(failure) && failedLaunches == 1, "Shell failure is reported and never retried with another target");
    DWORD bytes = 0;
    GetFileSecurityW(target.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &bytes);
    std::vector<BYTE> previous(bytes);
    Check(bytes != 0 && GetFileSecurityW(target.c_str(), DACL_SECURITY_INFORMATION,
        previous.data(), bytes, &bytes), "Save only our synthetic directory permissions");
    PSECURITY_DESCRIPTOR denied = nullptr;
    Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x1;;;WD)(A;;FA;;;WD)",
        SDDL_REVISION_1, &denied, nullptr), "Construct synthetic list-access denial");
    const bool changed = SetFileSecurityW(target.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, denied) != FALSE;
    LocalFree(denied);
    Check(changed, "Apply denial only to the explicitly created test directory");
    const auto inaccessible = Space::InstallDirectory::Open(target.wstring(), nullptr, capture);
    const bool restored = SetFileSecurityW(target.c_str(), DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION, previous.data()) != FALSE;
    Check(restored, "Restore synthetic directory permissions");
    Check(FAILED(inaccessible) && calls == 1, "Inaccessible directory is refused before Shell with no fallback");
}
static int ShellFixture()
{
    wchar_t cloud[8]{};
    Check(GetEnvironmentVariableW(L"GITHUB_ACTIONS", cloud, 8) != 0 && std::wstring(cloud) == L"true",
        "Actual Explorer verification is cloud-only");
    const auto root = DirectoryFixture();
    const auto target = root / L"Copilot" / L"Application";
    Check(SUCCEEDED(Space::InstallDirectory::Open(target.wstring(), nullptr)), "Open only a self-created synthetic directory");
    std::wcout << L"{\"target\":\"";
    for (const auto ch : target.wstring()) { if (ch == L'\\') std::wcout << L'\\'; std::wcout << ch; }
    std::wcout << L"\"}\n";
    return 0;
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Check(SUCCEEDED(com), "Initialize COM for folder identity operations");
        struct ComThread { ~ComThread() { CoUninitialize(); } } comThread;
        if (argc == 2 && std::wstring(argv[1]) == L"--shell-fixture") return ShellFixture();
        Check(argc == 1, "Only the explicit synthetic Shell mode is supported");
        DirectoryRequests();
        IA::Record record{ IA::Source::Machine64, L"key", L"Synthetic App", L"Publisher", L"2", L"C:\\SyntheticOnly", 2048 };
        const auto converted = IA::Convert(record);
        Check(converted && converted->estimatedBytes == 2097152, "Registry KiB converted to bytes without measurement");
        record.estimatedKiB = 0;
        Check(!IA::Convert(record)->estimatedBytes, "Zero reported size is unknown");
        record.estimatedKiB = 0xffffffffu;
        Check(IA::Convert(record)->estimatedBytes == std::uint64_t(0xffffffffu) * 1024, "Large DWORD does not overflow");
        record.source = IA::Source::Msix;
        Check(!IA::Convert(record)->estimatedBytes, "MSIX metadata does not inherit a registry size estimate");
        record.installLocation = L"C:\\Synthetic\nDifferent";
        Check(IA::Convert(record)->installLocation.empty(), "Control characters cannot turn into a different clickable path");
        record.name.clear();
        Check(!IA::Convert(record), "Nameless records do not invent an application name");

        IA::Item large{ IA::Source::Machine64, L"a", L"Alpha", L"Vendor", L"1", L"C:\\One", 4096 };
        IA::Item smallerApp{ IA::Source::Machine32, L"b", L"Beta", L"Other", L"2", L"C:\\Two", 1024 };
        IA::Item unknown{ IA::Source::Msix, L"c", L"中文 App", L"Vendor", L"3", L"", std::nullopt };
        std::vector<IA::Item> items{ smallerApp, unknown, large };
        Check(IA::Select(items, L"", true) == std::vector<size_t>({ 2, 0, 1 }), "Descending numerical size, unknown last");
        Check(IA::Select(items, L"", false) == std::vector<size_t>({ 0, 2, 1 }), "Ascending keeps unknown last");
        Check(IA::Select(items, L"aLpHa", true) == std::vector<size_t>({ 2 }), "Search ignores case");
        Check(IA::Select(items, L"中文", true) == std::vector<size_t>({ 1 }), "Search preserves Unicode");
        Check(IA::Select(items, L"Vendor", true).size() == 2, "Search includes publisher");
        Check(IA::Select(items, L"C:\\Two", true) == std::vector<size_t>({ 0 }), "Search includes install location");
        Check(IA::Select(items, L"*", true).empty(), "Search text is not a wildcard or executable expression");
        Check(IA::Select(items, L"missing", true).empty(), "Empty search result is valid");

        GUID guid{};
        Check(SUCCEEDED(CoCreateGuid(&guid)), "Generate unique fixture identity");
        wchar_t guidText[40]{};
        StringFromGUID2(guid, guidText, 40);
        const auto root = std::wstring(L"Software\\WinDirStatTests\\InstalledApps-") + std::wstring(guidText + 1, 36);
        Key fixture;
        Check(RegCreateKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE,
            nullptr, &fixture.value, nullptr) == ERROR_SUCCESS, "Create explicitly owned fixture root");
        String(fixture.value, L"WDSInstalledAppsFixture", L"v1");
        for (const auto* source : { L"Machine32", L"Machine64", L"User32", L"User64", L"Msix" })
        {
            Key sourceKey;
            Check(RegCreateKeyExW(fixture.value, source, 0, nullptr, 0, KEY_READ | KEY_WRITE,
                nullptr, &sourceKey.value, nullptr) == ERROR_SUCCESS, "Create synthetic source");
        }
        App(root, L"Machine32", L"same-name", L"Same App", 1024);
        App(root, L"Machine64", L"same-name", L"Same App", 2048);
        App(root, L"User32", L"shared-key", L"Current User App", 512);
        App(root, L"User64", L"shared-key", L"Current User App", 512);
        App(root, L"Machine64", L"bad-size", L"Unknown Size", 0, true);
        App(root, L"Msix", L"synthetic-package", L"Packaged App", 9999);
        const auto loaded = IA::LoadFixtureRegistry(root);
        Check(!loaded.invalidFixture && !loaded.cancelled, "Only valid marked synthetic sources are read");
        Check(loaded.items.size() == 5, "Shared HKCU views deduplicate; different machine views remain");
        for (const auto& source : loaded.sources) Check(source.state == (source.source == IA::Source::Machine64 ? IA::State::Partial : IA::State::Complete), "Malformed source is partial; other synthetic sources complete");
        const auto sameNames = IA::Select(loaded.items, L"Same App", true);
        Check(sameNames.size() == 2, "Same display name does not merge separate installations");
        const auto malformed = IA::Select(loaded.items, L"Unknown Size", true);
        Check(malformed.size() == 1 && !loaded.items[malformed[0]].estimatedBytes, "Malformed registry size stays unknown");
        const auto package = IA::Select(loaded.items, L"Packaged App", true);
        Check(package.size() == 1 && !loaded.items[package[0]].estimatedBytes, "Synthetic MSIX size stays unknown");
        Check(IA::Select(loaded.items, L"NEVER_EXECUTE", true).empty(), "Uninstall command never enters displayed metadata");
        std::stop_source stopped;
        stopped.request_stop();
        Check(IA::LoadFixtureRegistry(root, stopped.get_token()).cancelled, "Pre-cancelled enumeration stops");
        Check(IA::LoadFixtureRegistry(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall").invalidFixture,
            "A real Uninstall path is rejected by the fixture API without enumeration");
        String(fixture.value, L"WDSInstalledAppsFixture", L"invalid");
        const auto invalid = IA::LoadFixtureRegistry(root);
        Check(invalid.invalidFixture && invalid.items.empty(), "Invalid fixture never falls back to real inventory");
        std::cout << "{\"status\":\"PASS\",\"checks\":" << checks << ",\"real_inventory_queries\":0,\"uninstall_calls\":0}\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
