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
int wmain(int argc, wchar_t** argv)
{
    try {
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
        std::cout << "{\"status\":\"PASS\",\"checks\":" << checks << "}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Cleanup preview test failed: " << error.what() << std::endl;
        return 1;
    }
}
