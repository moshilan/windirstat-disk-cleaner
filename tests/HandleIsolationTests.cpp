// Test-only handle-rename evaluation. All objects are created here; no Shell or deletion.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <memory>
#include <array>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <stop_token>
#include "Cleanup/CleanupPreview.h"
namespace fs = std::filesystem;
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
static unsigned checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
static Handle Open(const fs::path& path, DWORD access, DWORD share, DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT)
{
    HANDLE h = CreateFileW((L"\\\\?\\" + path.wstring()).c_str(), access, share, nullptr, OPEN_EXISTING, flags, nullptr);
    return Handle(h == INVALID_HANDLE_VALUE ? nullptr : h, CloseHandle);
}
struct Identity
{
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
};
static Identity ReadIdentity(HANDLE file)
{
    Identity value;
    Check(GetFileInformationByHandleEx(file, FileIdInfo, &value.id, sizeof(value.id)) &&
        GetFileInformationByHandleEx(file, FileStandardInfo, &value.standard, sizeof(value.standard)), "Read full volume and 128-bit file identity");
    return value;
}
static bool Same(HANDLE file, const Identity& expected)
{
    if (!file) return false;
    const auto value = ReadIdentity(file);
    return value.id.VolumeSerialNumber == expected.id.VolumeSerialNumber &&
        memcmp(value.id.FileId.Identifier, expected.id.FileId.Identifier, sizeof(value.id.FileId.Identifier)) == 0 &&
        value.standard.NumberOfLinks == 1 && !value.standard.Directory && !value.standard.DeletePending &&
        value.standard.EndOfFile.QuadPart == expected.standard.EndOfFile.QuadPart &&
        value.standard.AllocationSize.QuadPart == expected.standard.AllocationSize.QuadPart;
}
static bool SameDirectory(HANDLE directory, const Identity& expected)
{
    if (!directory) return false;
    const auto value = ReadIdentity(directory);
    return value.standard.Directory && value.id.VolumeSerialNumber == expected.id.VolumeSerialNumber &&
        memcmp(value.id.FileId.Identifier, expected.id.FileId.Identifier, sizeof(value.id.FileId.Identifier)) == 0;
}
static bool SafeDirectory(HANDLE directory)
{
    FILE_ATTRIBUTE_TAG_INFO info{};
    return directory && GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &info, sizeof(info)) &&
        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
static bool FinalPath(HANDLE handle, const fs::path& expected)
{
    std::wstring path(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle, path.data(), DWORD(path.size()), FILE_NAME_NORMALIZED);
    if (!length || length >= path.size()) return false;
    path.resize(length);
    return _wcsicmp(path.c_str(), (L"\\\\?\\" + expected.wstring()).c_str()) == 0;
}
static DWORD Rename(HANDLE source, HANDLE directory, const std::wstring& name)
{
    const auto bytes = name.size() * sizeof(wchar_t);
    std::vector<BYTE> data(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t), 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(data.data());
    rename->RootDirectory = directory; rename->FileNameLength = DWORD(bytes);
    memcpy(rename->FileName, name.data(), bytes);
    // ReplaceIfExists is always FALSE. No POSIX, disposition, overwrite or fallback flags.
    return SetFileInformationByHandle(source, FileRenameInfo, rename, DWORD(data.size())) ? ERROR_SUCCESS : GetLastError();
}
static void Print(const char* name, DWORD error, bool sourceMatches, bool called = true)
{
    std::cout << "{\"case\":\"" << name << "\",\"error\":" << error << ",\"sourceIdentityMatches\":" << sourceMatches
        << ",\"renameCalled\":" << called << ",\"controlOnly\":true,\"readyForRealFiles\":false}\n";
}
class Fixture
{
public:
    struct Receipt { fs::path path; Identity identity; Handle retained{ nullptr, CloseHandle }; };
    fs::path root, source, quarantine, holding, decoy;
    Handle sourceDirectory{ nullptr, CloseHandle }, targetDirectory{ nullptr, CloseHandle };
    Fixture()
    {
        wchar_t temp[32768]{}, guidText[40]{}; GUID guid{};
        const DWORD length = GetTempPathW(32768, temp);
        Check(length && length < 32768 && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, guidText, 40), "Generate fresh fixture");
        std::wstring normalized, error;
        Check(Cleanup::NormalizeDirectory(temp, normalized, error), "Normalize temporary directory");
        const auto drive = fs::path(normalized).root_path(); wchar_t fileSystem[32]{};
        Check(GetDriveTypeW(drive.c_str()) == DRIVE_FIXED && GetVolumeInformationW(drive.c_str(), nullptr, 0,
            nullptr, nullptr, nullptr, fileSystem, 32) && _wcsicmp(fileSystem, L"NTFS") == 0, "Fixed local NTFS only");
        PinChain(normalized);
        root = fs::path(normalized) / (L"WinDirStatHandleIsolation-" + std::wstring(guidText));
        Check(CreateDirectoryW(root.c_str(), nullptr) != FALSE, "Root must be newly created");
        PinChain(root);
        source = root / L"source"; quarantine = root / L"quarantine";
        holding = root / L"holding"; decoy = root / L"decoy";
        for (const auto& directory : { source, quarantine, holding, decoy })
            Check(CreateDirectoryW(directory.c_str(), nullptr) != FALSE, "Create owned experiment directory");
        sourceDirectory = Directory(source); targetDirectory = Directory(quarantine);
        Check(SafeDirectory(sourceDirectory.get()) && SafeDirectory(targetDirectory.get()), "Bind initial leaf directory handles");
        sourceDirectoryIdentity = ReadIdentity(sourceDirectory.get()); targetDirectoryIdentity = ReadIdentity(targetDirectory.get());
    }
    // Leaf WRITE sharing is declared from the start for kernel rename destination opens.
    // It permits reparse writers; these experimental handles grant no production capability.
    static Handle Directory(const fs::path& path, DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE)
    { return Open(path, GENERIC_READ | FILE_TRAVERSE, share, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS); }
    size_t NewSource() { return CreateAt(source / L"owned.tmp"); }
    size_t NewConflict() { return CreateAt(quarantine / L"isolated.tmp"); }
    const Receipt& Get(size_t index) const { return receipts.at(index); }
    Handle Operation(size_t index)
    {
        auto handle = Open(Get(index).path, GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        if (handle) Check(Same(handle.get(), Get(index).identity) && FinalPath(handle.get(), Get(index).path), "Operation handle matches original CREATE_NEW receipt");
        return handle;
    }
    bool At(const fs::path& path, const Identity& expected) const
    {
        auto file = Open(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        return file && Same(file.get(), expected);
    }
    DWORD Rooted(HANDLE file, std::stop_token stop = {})
    {
        if (stop.stop_requested()) return ERROR_CANCELLED;
        if (!SafeDirectory(targetDirectory.get())) return ERROR_REPARSE_TAG_INVALID;
        const auto fileIdentity = ReadIdentity(file), directoryIdentity = ReadIdentity(targetDirectory.get());
        if (fileIdentity.id.VolumeSerialNumber != directoryIdentity.id.VolumeSerialNumber) return ERROR_NOT_SAME_DEVICE;
        return Rename(file, targetDirectory.get(), L"isolated.tmp");
    }
    // Independent absolute-name API controls. NEVER called by Rooted(), nor as its fallback.
    DWORD AbsoluteControl(HANDLE file, const fs::path& target) const { return Rename(file, nullptr, target.wstring()); }
    size_t ReplaceSource(HANDLE original)
    {
        Check(AbsoluteControl(original, holding / L"original.tmp") == 0, "Move original using its verified handle");
        return CreateAt(source / L"owned.tmp");
    }
    void JunctionTarget()
    {
        const std::wstring substitute = L"\\??\\" + decoy.wstring();
        struct Data { DWORD tag; WORD length, reserved, subOffset, subLength, printOffset, printLength; wchar_t paths[32768]; } data{};
        data.tag = IO_REPARSE_TAG_MOUNT_POINT; data.subLength = WORD(substitute.size() * sizeof(wchar_t));
        data.printOffset = WORD(data.subLength + sizeof(wchar_t)); data.length = WORD(8 + data.printOffset + sizeof(wchar_t));
        memcpy(data.paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
        auto writer = Open(quarantine, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS);
        DWORD returned = 0;
        Check(writer && DeviceIoControl(writer.get(), FSCTL_SET_REPARSE_POINT, &data, DWORD(data.length) + 8,
            nullptr, 0, &returned, nullptr), "Install junction only in owned empty target");
    }
    void CloseForInterruptedObservation()
    {
        for (auto& item : receipts) item.retained.reset();
        targetDirectory.reset(); sourceDirectory.reset(); pins.clear();
    }
    bool RepinForRecovery()
    {
        PinChain(root);
        sourceDirectory = Directory(source); targetDirectory = Directory(quarantine);
        return SafeDirectory(sourceDirectory.get()) && SafeDirectory(targetDirectory.get()) &&
            SameDirectory(sourceDirectory.get(), sourceDirectoryIdentity) && SameDirectory(targetDirectory.get(), targetDirectoryIdentity);
    }
private:
    std::vector<Receipt> receipts;
    std::vector<Handle> pins;
    Identity sourceDirectoryIdentity, targetDirectoryIdentity;
    void PinChain(const fs::path& directory)
    {
        auto path = directory.root_path();
        const auto pin = [&](const fs::path& component)
        {
            auto handle = Directory(component, FILE_SHARE_READ);
            Check(SafeDirectory(handle.get()) && FinalPath(handle.get(), component), "Pin ordinary ancestor identity and normalized path");
            pins.push_back(std::move(handle));
        };
        pin(path);
        for (const auto& part : directory.relative_path()) { path /= part; pin(path); }
    }
    size_t CreateAt(const fs::path& path)
    {
        Receipt item; item.path = path;
        HANDLE value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        Check(value != INVALID_HANDLE_VALUE, "CREATE_NEW owned file");
        Handle writer(value, CloseHandle); const char payload[] = "synthetic handle isolation payload"; DWORD written = 0;
        Check(WriteFile(writer.get(), payload, sizeof(payload), &written, nullptr) && written == sizeof(payload) && FlushFileBuffers(writer.get()), "Write owned payload");
        item.identity = ReadIdentity(writer.get());
        HANDLE retained = ReOpenFile(writer.get(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
        Check(retained != INVALID_HANDLE_VALUE, "Retain original object without name reopen");
        item.retained.reset(retained);
        Check(Same(item.retained.get(), item.identity), "Retained receipt matches creating handle");
        receipts.push_back(std::move(item)); return receipts.size() - 1;
    }
};
class RestrictedScope
{
public:
    RestrictedScope()
    {
        HANDLE process = nullptr, restricted = nullptr; BYTE sid[SECURITY_MAX_SID_SIZE]{}; DWORD size = sizeof(sid);
        Check(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, &process) != FALSE, "Open own token");
        bool ok = CreateWellKnownSid(WinRestrictedCodeSid, nullptr, sid, &size) != FALSE;
        SID_AND_ATTRIBUTES restriction{ sid, 0 };
        if (ok) ok = CreateRestrictedToken(process, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1, &restriction, &restricted) != FALSE;
        CloseHandle(process);
        if (ok) ok = ImpersonateLoggedOnUser(restricted) != FALSE;
        if (restricted) CloseHandle(restricted);
        Check(ok, "Restricted impersonation without ACL writes or elevation");
    }
    ~RestrictedScope() { if (!RevertToSelf()) std::terminate(); }
};
static void Run()
{
    {
        Fixture fixture; const auto index = fixture.NewSource(); auto file = fixture.Operation(index);
        const DWORD error = fixture.Rooted(file.get());
        Print("win32_root_directory", error, Same(file.get(), fixture.Get(index).identity));
        if (!error)
        {
            Check(fixture.At(fixture.quarantine / L"isolated.tmp", fixture.Get(index).identity), "Supported rooted rename preserves identity");
            Check(Rename(file.get(), fixture.sourceDirectory.get(), L"owned.tmp") == 0, "Supported rooted restore");
        }
        else Check(error == ERROR_INVALID_PARAMETER && fixture.At(fixture.Get(index).path, fixture.Get(index).identity) &&
            !fs::exists(fixture.quarantine / L"isolated.tmp"), "Unsupported rooted API leaves source and target untouched");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(); auto file = fixture.Operation(index);
        const auto identity = fixture.Get(index).identity;
        const DWORD error = fixture.AbsoluteControl(file.get(), fixture.quarantine / L"isolated.tmp");
        Print("absolute_normal_CONTROL", error, Same(file.get(), identity));
        Check(error == 0 && fixture.At(fixture.quarantine / L"isolated.tmp", identity) && !fs::exists(fixture.Get(index).path), "Source-bound control isolates same full ID and allocation");
        Check(fixture.AbsoluteControl(file.get(), fixture.source / L"owned.tmp") == 0 && fixture.At(fixture.source / L"owned.tmp", identity), "Explicit restore preserves identity and allocation");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(); auto file = fixture.Operation(index);
        const auto identity = fixture.Get(index).identity; const auto replacement = fixture.ReplaceSource(file.get());
        const DWORD error = fixture.AbsoluteControl(file.get(), fixture.quarantine / L"isolated.tmp");
        Print("source_replaced_CONTROL", error, Same(file.get(), identity));
        Check(error == 0 && fixture.At(fixture.quarantine / L"isolated.tmp", identity) && fixture.At(fixture.source / L"owned.tmp", fixture.Get(replacement).identity), "Handle moves original, never same-name replacement");
        const DWORD restore = fixture.AbsoluteControl(file.get(), fixture.source / L"owned.tmp");
        Print("restore_conflict_CONTROL", restore, Same(file.get(), identity));
        Check((restore == ERROR_ALREADY_EXISTS || restore == ERROR_FILE_EXISTS) &&
            fixture.At(fixture.quarantine / L"isolated.tmp", identity) && fixture.At(fixture.source / L"owned.tmp", fixture.Get(replacement).identity), "Restore does not overwrite new same-name file");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(), conflict = fixture.NewConflict(); auto file = fixture.Operation(index);
        const DWORD error = fixture.AbsoluteControl(file.get(), fixture.quarantine / L"isolated.tmp");
        Print("target_conflict_CONTROL", error, Same(file.get(), fixture.Get(index).identity));
        Check((error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) &&
            fixture.At(fixture.Get(index).path, fixture.Get(index).identity) && fixture.At(fixture.Get(conflict).path, fixture.Get(conflict).identity), "Target conflict preserves both objects");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(); auto file = fixture.Operation(index);
        auto strict = Fixture::Directory(fixture.quarantine, FILE_SHARE_READ);
        Check(strict != nullptr, "Strict target sharing guard opened");
        const DWORD error = fixture.AbsoluteControl(file.get(), fixture.quarantine / L"isolated.tmp");
        Print("strict_target_guard_CONTROL", error, Same(file.get(), fixture.Get(index).identity));
        Check(error == ERROR_SHARING_VIOLATION && fixture.At(fixture.Get(index).path, fixture.Get(index).identity), "Do not loosen a directory guard to retry rename");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource();
        auto writer = Open(fixture.Get(index).path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE);
        Check(writer != nullptr, "Existing writer refuses delete sharing");
        auto operation = fixture.Operation(index); const DWORD error = GetLastError();
        Print("file_occupied", error, fixture.At(fixture.Get(index).path, fixture.Get(index).identity), false);
        Check(!operation && error == ERROR_SHARING_VIOLATION, "Occupied source blocked before mutation");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(); DWORD error = 0;
        { RestrictedScope restricted; auto operation = fixture.Operation(index); error = GetLastError(); Check(!operation, "Restricted source cannot acquire DELETE capability"); }
        Print("source_permission", error, fixture.At(fixture.Get(index).path, fixture.Get(index).identity), false);
        Check(error == ERROR_ACCESS_DENIED, "Source permission failure is real access denied");
        auto operation = fixture.Operation(index);
        { RestrictedScope restricted; error = fixture.AbsoluteControl(operation.get(), fixture.quarantine / L"isolated.tmp"); }
        Print("target_permission_CONTROL", error, Same(operation.get(), fixture.Get(index).identity));
        Check(error == ERROR_ACCESS_DENIED && fixture.At(fixture.Get(index).path, fixture.Get(index).identity), "Destination permission still checked with existing source capability");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(); auto file = fixture.Operation(index); fixture.JunctionTarget();
        const DWORD error = fixture.Rooted(file.get());
        Print("preexisting_reparse_blocked", error, Same(file.get(), fixture.Get(index).identity), false);
        Check(error == ERROR_REPARSE_TAG_INVALID && fixture.At(fixture.Get(index).path, fixture.Get(index).identity) && fs::is_empty(fixture.decoy), "Reject target reparse point before rename");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(); auto file = fixture.Operation(index);
        Check(SafeDirectory(fixture.targetDirectory.get()), "Target ordinary at final precheck");
        fixture.JunctionTarget();
        const DWORD error = fixture.AbsoluteControl(file.get(), fixture.quarantine / L"isolated.tmp");
        Print("target_reparse_after_check_CONTROL", error, Same(file.get(), fixture.Get(index).identity));
        Check(error == 0 && fixture.At(fixture.decoy / L"isolated.tmp", fixture.Get(index).identity) && FinalPath(file.get(), fixture.decoy / L"isolated.tmp"), "Absolute target path can be redirected despite source handle binding");
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(); auto file = fixture.Operation(index);
        std::stop_source stop; stop.request_stop();
        const DWORD error = fixture.Rooted(file.get(), stop.get_token());
        Print("cancel_before_commit", error, Same(file.get(), fixture.Get(index).identity), false);
        Check(error == ERROR_CANCELLED && fixture.At(fixture.Get(index).path, fixture.Get(index).identity), "Cancel before commit leaves source intact");
        const auto expected = fixture.Get(index).identity;
        Check(fixture.AbsoluteControl(file.get(), fixture.quarantine / L"isolated.tmp") == 0, "Commit isolated control before interrupted result publication");
        file.reset(); fixture.CloseForInterruptedObservation();
        // Deterministic observation interruption, NOT a process-kill/power-loss durability test.
        Check(fixture.RepinForRecovery(), "Recovery revalidates source and target directory identities");
        Check(!fs::exists(fixture.source / L"owned.tmp") && fixture.At(fixture.quarantine / L"isolated.tmp", expected), "Unknown result reconciled by identity, never by name alone");
        auto recovered = Open(fixture.quarantine / L"isolated.tmp", GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        Check(recovered && Same(recovered.get(), expected), "Reacquire exact object before explicit restore");
        Check(fixture.AbsoluteControl(recovered.get(), fixture.source / L"owned.tmp") == 0 && fixture.At(fixture.source / L"owned.tmp", expected), "Explicit recovery control restores exact same object");
        Print("interrupted_result_reconciled_CONTROL", 0, Same(recovered.get(), expected));
    }
    {
        Fixture fixture; const auto index = fixture.NewSource(); const auto expected = fixture.Get(index).identity;
        fixture.CloseForInterruptedObservation();
        fs::rename(fixture.source, fixture.holding / L"old-source");
        Check(CreateDirectoryW(fixture.source.c_str(), nullptr) != FALSE, "Create owned replacement source parent");
        Check(!fixture.RepinForRecovery() && fixture.At(fixture.holding / L"old-source" / L"owned.tmp", expected) &&
            fs::is_empty(fixture.source), "Changed parent identity blocks recovery without touching either directory");
        Print("recovery_parent_replaced_blocked", ERROR_FILE_INVALID, true, false);
    }
    std::cout << "{\"checks\":" << checks << ",\"failures\":0,\"readyForRealFiles\":false,\"permanentDeleteImplemented\":false,"
        "\"releasedBytesCounted\":0,\"processCrashTested\":false,\"automaticFallback\":false}\n";
}
int wmain(int argc, wchar_t**)
{
    if (argc != 1) { std::cerr << "No external path or command-line input accepted\n"; return 2; }
    int code = 0;
    try { Run(); }
    catch (const std::exception& error) { std::cerr << "Check " << checks << " failed: " << error.what() << '\n'; code = 1; }
    return code;
}
