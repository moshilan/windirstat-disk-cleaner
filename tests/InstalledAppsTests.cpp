// Synthetic installed-application metadata only. Never enumerate a real Uninstall key or MSIX inventory.
#include <windows.h>
#include <objbase.h>
#include <iostream>
#include <stdexcept>
#include "Space/InstalledApps.h"
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
int wmain()
{
    try
    {
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
