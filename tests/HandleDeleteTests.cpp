// Test-only, irreversible disposition of CREATE_NEW receipts. Never linked into WinDirStat.
// Reuses the handle-isolation experiment's ownership, ancestor-pin and restricted-token patterns.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <winioctl.h>
#include <filesystem>
#include <memory>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <functional>
#include <stop_token>
#include "Cleanup/CleanupPreview.h"
namespace fs = std::filesystem;
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
static unsigned checks = 0, cases = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
static Handle Open(const fs::path& path, DWORD access, DWORD share, DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT)
{
    HANDLE h = CreateFileW((L"\\\\?\\" + path.wstring()).c_str(), access, share, nullptr, OPEN_EXISTING, flags, nullptr);
    return Handle(h == INVALID_HANDLE_VALUE ? nullptr : h, CloseHandle);
}
struct Snapshot { FILE_ID_INFO id{}; FILE_STANDARD_INFO standard{}; FILE_BASIC_INFO basic{}; FILE_ATTRIBUTE_TAG_INFO tag{}; };
static bool Read(HANDLE h, Snapshot& s)
{
    return GetFileInformationByHandleEx(h, FileIdInfo, &s.id, sizeof(s.id)) &&
        GetFileInformationByHandleEx(h, FileStandardInfo, &s.standard, sizeof(s.standard)) &&
        GetFileInformationByHandleEx(h, FileBasicInfo, &s.basic, sizeof(s.basic)) &&
        GetFileInformationByHandleEx(h, FileAttributeTagInfo, &s.tag, sizeof(s.tag));
}
static bool SameId(const Snapshot& a, const Snapshot& b)
{
    return a.id.VolumeSerialNumber == b.id.VolumeSerialNumber &&
        memcmp(a.id.FileId.Identifier, b.id.FileId.Identifier, sizeof(a.id.FileId.Identifier)) == 0;
}
static bool SameData(const Snapshot& a, const Snapshot& b)
{
    return a.standard.EndOfFile.QuadPart == b.standard.EndOfFile.QuadPart &&
        a.standard.AllocationSize.QuadPart == b.standard.AllocationSize.QuadPart &&
        a.basic.CreationTime.QuadPart == b.basic.CreationTime.QuadPart &&
        a.basic.LastWriteTime.QuadPart == b.basic.LastWriteTime.QuadPart &&
        a.basic.ChangeTime.QuadPart == b.basic.ChangeTime.QuadPart && a.basic.FileAttributes == b.basic.FileAttributes;
}
static bool FinalPath(HANDLE h, const fs::path& expected)
{
    std::wstring value(32768, L'\0');
    const DWORD size = GetFinalPathNameByHandleW(h, value.data(), DWORD(value.size()), FILE_NAME_NORMALIZED);
    if (!size || size >= value.size()) return false;
    value.resize(size); return _wcsicmp(value.c_str(), (L"\\\\?\\" + expected.wstring()).c_str()) == 0;
}
static bool OnlyDefaultStream(HANDLE h)
{
    // Enumerate on the deletion handle, never FindFirstStream(path) after validation.
    for (DWORD size = 4096; size <= 1024 * 1024; size *= 2)
    {
        std::vector<BYTE> buffer(size);
        if (GetFileInformationByHandleEx(h, FileStreamInfo, buffer.data(), size))
        {
            const auto* stream = reinterpret_cast<const FILE_STREAM_INFO*>(buffer.data());
            return stream->NextEntryOffset == 0 && stream->StreamNameLength == 7 * sizeof(wchar_t) &&
                memcmp(stream->StreamName, L"::$DATA", 7 * sizeof(wchar_t)) == 0;
        }
        if (GetLastError() != ERROR_MORE_DATA && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    }
    return false;
}
enum class State { Rejected, Cancelled, MarkedPending, NameAbsent, Unknown };
enum class Reason { None, Access, Ancestor, Identity, Changed, Attributes, Links, Streams, Cancelled, Disposition, Observe };
struct Result { State state = State::Rejected; Reason reason = Reason::None; DWORD error = 0; bool called = false, pending = false; };
static void Print(const char* name, const Result& r)
{
    ++cases;
    std::cout << "{\"case\":\"" << name << "\",\"state\":" << int(r.state) << ",\"reason\":" << int(r.reason)
        << ",\"error\":" << r.error << ",\"dispositionCalled\":" << r.called << ",\"deletePending\":" << r.pending
        << ",\"readyForRealFiles\":false,\"actualReleasedBytes\":0}\n";
}
class Fixture
{
public:
    struct Receipt { fs::path path; Snapshot original; Handle retained{ nullptr, CloseHandle }; };
    fs::path root, source, holding, decoy;
    Fixture()
    {
        wchar_t temp[32768]{}, name[40]{}; GUID guid{};
        const DWORD size = GetTempPathW(32768, temp);
        Check(size && size < 32768 && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, name, 40), "Generate fresh fixture");
        std::wstring normalized, error;
        Check(Cleanup::NormalizeDirectory(temp, normalized, error), "Normalize own fixture parent");
        const auto drive = fs::path(normalized).root_path(); wchar_t system[32]{};
        Check(GetDriveTypeW(drive.c_str()) == DRIVE_FIXED && GetVolumeInformationW(drive.c_str(), nullptr, 0,
            nullptr, nullptr, nullptr, system, 32) && _wcsicmp(system, L"NTFS") == 0, "Fixed local NTFS only");
        Check(PinChain(normalized), "Pin all ordinary ancestors before fixture creation");
        root = fs::path(normalized) / (L"WinDirStatHandleDelete-" + std::wstring(name));
        Check(CreateDirectoryW(root.c_str(), nullptr) && PinChain(root), "Fresh root with pinned identity");
        source = root / L"source"; holding = root / L"holding"; decoy = root / L"decoy";
        for (const auto& path : { source, holding, decoy }) Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "Create owned directory");
    }
    // No recursive cleanup. Only Execute() may dispose of individual creating-handle receipts.
    size_t Create(bool alternateStream = false) { return CreateAt(source / (std::to_wstring(receipts.size()) + L".cache"), alternateStream); }
    bool BindSource() { if (sourceBound) return SafePins(); sourceBound = PinChain(source); return sourceBound; }
    const Receipt& Get(size_t i) const { return receipts.at(i); }
    bool Present(size_t i, const fs::path& path = {}) const
    {
        auto h = Open(path.empty() ? Get(i).path : path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        Snapshot s; return h && Read(h.get(), s) && SameId(s, Get(i).original) && !s.standard.DeletePending;
    }
    size_t Replace(size_t i)
    {
        auto h = Open(Get(i).path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        Check(h && Rename(h.get(), holding / L"original.cache") == 0, "Move only own original before acquisition");
        return CreateAt(Get(i).path);
    }
    static DWORD Rename(HANDLE h, const fs::path& target)
    {
        const auto name = target.wstring(); const auto bytes = name.size() * sizeof(wchar_t);
        std::vector<BYTE> data(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t), 0);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(data.data()); info->FileNameLength = DWORD(bytes);
        memcpy(info->FileName, name.data(), bytes);
        return SetFileInformationByHandle(h, FileRenameInfo, info, DWORD(data.size())) ? 0 : GetLastError();
    }
    Result Execute(size_t i, std::stop_token stop = {}, const std::function<void(HANDLE)>& beforeFinalReview = {}, bool keepReceipt = false)
    {
        auto& receipt = receipts.at(i); Result result;
        const auto reject = [&](Reason reason, DWORD error = 0) { result.reason = reason; result.error = error; return result; };
        if (stop.stop_requested()) { result.state = State::Cancelled; return reject(Reason::Cancelled, ERROR_CANCELLED); }
        if (!BindSource()) return reject(Reason::Ancestor, GetLastError());
        // DELETE capability is acquired once. Strong sharing is never relaxed on failure.
        auto h = Open(receipt.path, GENERIC_READ | DELETE, FILE_SHARE_READ);
        if (!h) return reject(Reason::Access, GetLastError());
        if (beforeFinalReview) beforeFinalReview(h.get());
        Snapshot s, retained;
        if (!Read(h.get(), s) || !Read(receipt.retained.get(), retained)) return reject(Reason::Access, GetLastError());
        if (!SameId(s, receipt.original) || !SameId(s, retained)) return reject(Reason::Identity);
        if (!SafePins() || !FinalPath(h.get(), receipt.path)) return reject(Reason::Ancestor);
        constexpr DWORD forbidden = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_READONLY |
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_SPARSE_FILE |
            FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
        if (s.standard.Directory || s.standard.DeletePending || (s.tag.FileAttributes & forbidden)) return reject(Reason::Attributes);
        if (s.standard.NumberOfLinks != 1) return reject(Reason::Links);
        if (!SameData(s, receipt.original)) return reject(Reason::Changed);
        if (!OnlyDefaultStream(h.get())) return reject(Reason::Streams);
        Snapshot final;
        if (!Read(h.get(), final) || !SameId(s, final) || !SameData(s, final) || final.standard.NumberOfLinks != 1 ||
            final.standard.DeletePending || !SafePins() || !FinalPath(h.get(), receipt.path)) return reject(Reason::Changed);
        if (stop.stop_requested()) { result.state = State::Cancelled; return reject(Reason::Cancelled, ERROR_CANCELLED); }
        // Commit point: EXACT same handle as all reviews above; no path reopen, force flags or fallback.
        FILE_DISPOSITION_INFO disposition{ TRUE }; result.called = true;
        if (!SetFileInformationByHandle(h.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
            return reject(Reason::Disposition, GetLastError());
        result.state = State::MarkedPending;
        Snapshot pending;
        result.pending = Read(h.get(), pending) && SameId(pending, receipt.original) && pending.standard.DeletePending;
        if (!result.pending) { result.state = State::Unknown; result.reason = Reason::Observe; }
        // No cancellation rollback after commit; successful disposition is irreversible at this boundary.
        h.reset();
        if (keepReceipt) return result;
        receipt.retained.reset();
        if (result.state == State::Unknown) return result;
        SetLastError(0); const auto attr = GetFileAttributesW(receipt.path.c_str()); const auto error = GetLastError();
        if (attr == INVALID_FILE_ATTRIBUTES && error == ERROR_FILE_NOT_FOUND) result.state = State::NameAbsent;
        else { result.state = State::Unknown; result.reason = Reason::Observe; result.error = error; }
        return result; // Namespace observation is NOT a free-space measurement or a durable completion log.
    }
    void CloseReceipt(size_t i) { receipts.at(i).retained.reset(); }
    void JunctionSource(bool leaf = false)
    {
        const std::wstring substitute = L"\\??\\" + decoy.wstring();
        struct Data { DWORD tag; WORD length, reserved, subOffset, subLength, printOffset, printLength; wchar_t paths[32768]; } data{};
        data.tag = IO_REPARSE_TAG_MOUNT_POINT; data.subLength = WORD(substitute.size() * sizeof(wchar_t));
        data.printOffset = WORD(data.subLength + sizeof(wchar_t)); data.length = WORD(8 + data.printOffset + sizeof(wchar_t));
        memcpy(data.paths, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
        const auto path = leaf ? source / L"0.cache" : source;
        if (leaf) Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "Create owned replacement leaf directory");
        auto h = Open(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS);
        DWORD returned = 0;
        Check(h && DeviceIoControl(h.get(), FSCTL_SET_REPARSE_POINT, &data, DWORD(data.length) + 8,
            nullptr, 0, &returned, nullptr), "Junction only inside own fixture");
    }
private:
    struct Pin { fs::path path; Snapshot identity; Handle handle{ nullptr, CloseHandle }; };
    std::vector<Receipt> receipts;
    std::vector<Pin> pins;
    bool sourceBound = false;
    bool PinChain(const fs::path& directory)
    {
        auto path = directory.root_path();
        const auto pin = [&](const fs::path& component)
        {
            Pin item; item.path = component;
            item.handle = Open(component, GENERIC_READ, FILE_SHARE_READ, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS);
            if (!item.handle || !Read(item.handle.get(), item.identity) || !item.identity.standard.Directory ||
                (item.identity.tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !FinalPath(item.handle.get(), component)) return false;
            pins.push_back(std::move(item)); return true;
        };
        if (!pin(path)) return false;
        for (const auto& part : directory.relative_path()) { path /= part; if (!pin(path)) return false; }
        return true;
    }
    bool SafePins() const
    {
        for (const auto& pin : pins)
        {
            Snapshot s;
            if (!Read(pin.handle.get(), s) || !SameId(s, pin.identity) || !s.standard.Directory ||
                (s.tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !FinalPath(pin.handle.get(), pin.path)) return false;
        }
        return true;
    }
    size_t CreateAt(const fs::path& path, bool alternateStream = false)
    {
        Receipt item; item.path = path;
        HANDLE value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        Check(value != INVALID_HANDLE_VALUE, "CREATE_NEW own synthetic payload"); Handle h(value, CloseHandle);
        const char payload[] = "synthetic regenerable cache handle disposition test"; DWORD written = 0;
        Check(WriteFile(h.get(), payload, sizeof(payload), &written, nullptr) && written == sizeof(payload) && FlushFileBuffers(h.get()), "Write owned payload");
        HANDLE retained = ReOpenFile(h.get(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
        Check(retained != INVALID_HANDLE_VALUE, "Retain creating object without path reopen"); item.retained.reset(retained);
        h.reset(); // Closing the writer finalizes timestamps before taking the experiment's plan snapshot.
        if (alternateStream)
        {
            HANDLE stream = CreateFileW((path.wstring() + L":owned").c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(stream != INVALID_HANDLE_VALUE, "Create own alternate stream before snapshot"); CloseHandle(stream);
        }
        Check(Read(item.retained.get(), item.original), "Capture full original identity and metadata");
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
        bool ok = CreateWellKnownSid(WinRestrictedCodeSid, nullptr, sid, &size) != FALSE; SID_AND_ATTRIBUTES restriction{ sid, 0 };
        if (ok) ok = CreateRestrictedToken(process, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1, &restriction, &restricted) != FALSE;
        CloseHandle(process); if (ok) ok = ImpersonateLoggedOnUser(restricted) != FALSE;
        if (restricted) CloseHandle(restricted); Check(ok, "Restricted token without ACL edits or elevation");
    }
    ~RestrictedScope() { if (!RevertToSelf()) std::terminate(); }
};
static void Run()
{
    {
        Fixture f; const auto i = f.Create(); const auto r = f.Execute(i); Print("normal_same_handle", r);
        Check(r.state == State::NameAbsent && r.called && r.pending, "Exact original marked and name absent after closing all experiment handles");
    }
    {
        Fixture f; const auto i = f.Create(), replacement = f.Replace(i); const auto r = f.Execute(i); Print("path_replaced_before_open", r);
        Check(!r.called && r.reason == Reason::Identity && f.Present(i, f.holding / L"original.cache") && f.Present(replacement), "Never delete same-name replacement");
    }
    {
        Fixture f; const auto i = f.Create(); DWORD renameError = 0;
        const auto r = f.Execute(i, {}, [&](HANDLE) { SetLastError(0); Check(!MoveFileExW(f.Get(i).path.c_str(), (f.holding / L"moved.cache").c_str(), 0), "External rename refused by guard"); renameError = GetLastError(); });
        Print("path_replace_blocked_while_guarded", r);
        Check(renameError == ERROR_SHARING_VIOLATION && r.state == State::NameAbsent, "Strict handle prevents external namespace replacement");
    }
    {
        Fixture f; const auto i = f.Create();
        const auto r = f.Execute(i, {}, [&](HANDLE h) { Check(Fixture::Rename(h, f.holding / L"moved.cache") == 0, "Own handle deliberately changes source name"); });
        Print("source_handle_renamed_before_review", r);
        Check(!r.called && r.reason == Reason::Ancestor && f.Present(i, f.holding / L"moved.cache"), "Changed final path refuses even matching ID");
    }
    {
        Fixture f; const auto i = f.Create();
        Check(CreateHardLinkW((f.holding / L"alias.cache").c_str(), f.Get(i).path.c_str(), nullptr) != FALSE, "Create own second link");
        const auto r = f.Execute(i); Print("hardlink_rejected", r);
        Check(!r.called && r.reason == Reason::Links && f.Present(i) && f.Present(i, f.holding / L"alias.cache"), "Preserve both hard links");
    }
    {
        Fixture f; const auto i = f.Create(); bool linked = false; DWORD linkError = 0;
        const auto r = f.Execute(i, {}, [&](HANDLE) { linked = CreateHardLinkW((f.holding / L"late.cache").c_str(), f.Get(i).path.c_str(), nullptr) != FALSE; if (!linked) linkError = GetLastError(); });
        Print(linked ? "late_hardlink_detected" : "late_hardlink_open_blocked", r);
        Check(linked ? (!r.called && r.reason == Reason::Links && f.Present(i)) :
            (linkError == ERROR_SHARING_VIOLATION && r.state == State::NameAbsent), "No mutation on observed extra link; never relax sharing");
    }
    {
        Fixture f; const auto i = f.Create();
        auto writer = Open(f.Get(i).path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE);
        Check(writer != nullptr, "Own occupied file"); const auto r = f.Execute(i); Print("occupied", r);
        Check(!r.called && r.reason == Reason::Access && r.error == ERROR_SHARING_VIOLATION && f.Present(i), "Occupied file preserved");
    }
    {
        Fixture f; const auto i = f.Create(); Result r;
        Check(f.BindSource(), "Bind ancestors before restricting file DELETE acquisition");
        { RestrictedScope restricted; r = f.Execute(i); } Print("permission_denied", r);
        Check(!r.called && r.reason == Reason::Access && r.error == ERROR_ACCESS_DENIED && f.Present(i), "DELETE acquisition failure preserves original without ACL changes");
    }
    {
        Fixture f; const auto i = f.Create();
        Check(SetFileAttributesW(f.Get(i).path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "Set own readonly fixture");
        const auto r = f.Execute(i); Print("readonly_rejected", r);
        Check(!r.called && r.reason == Reason::Attributes && f.Present(i), "No clearing attributes or force disposition");
    }
    {
        Fixture f; const auto i = f.Create();
        auto writer = Open(f.Get(i).path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        Check(writer != nullptr, "Open own change fixture"); LARGE_INTEGER size{}; size.QuadPart = 4096;
        Check(SetFilePointerEx(writer.get(), size, nullptr, FILE_BEGIN) && SetEndOfFile(writer.get()), "Change after snapshot"); writer.reset();
        const auto r = f.Execute(i); Print("changed_since_snapshot", r);
        Check(!r.called && r.reason == Reason::Changed && f.Present(i), "Changed file refused");
    }
    {
        Fixture f; const auto i = f.Create(true);
        const auto r = f.Execute(i); Print("alternate_stream_rejected", r);
        Check(!r.called && r.reason == Reason::Streams && f.Present(i), "Same-handle stream enumeration rejects preexisting ADS");
    }
    {
        Fixture f; const auto i = f.Create(); f.Replace(i); // source is now empty only after moving replacement too.
        auto replacement = Open(f.Get(i).path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        Check(replacement && Fixture::Rename(replacement.get(), f.holding / L"replacement.cache") == 0, "Empty own source for junction"); replacement.reset();
        f.JunctionSource(); const auto r = f.Execute(i); Print("ancestor_reparse_rejected", r);
        Check(!r.called && r.reason == Reason::Ancestor && fs::is_empty(f.decoy) && f.Present(i, f.holding / L"original.cache"), "Junction cannot redirect disposal");
    }
    {
        Fixture f; const auto i = f.Create(); const auto replacement = f.Replace(i);
        auto h = Open(f.Get(i).path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        Check(h && Fixture::Rename(h.get(), f.holding / L"replacement.cache") == 0, "Retain replacement before leaf reparse injection"); h.reset();
        f.JunctionSource(true); const auto r = f.Execute(i); Print("leaf_reparse_replacement_rejected", r);
        Check(!r.called && (r.reason == Reason::Identity || (r.reason == Reason::Access && r.error == ERROR_ACCESS_DENIED)) && fs::is_empty(f.decoy) &&
            f.Present(i, f.holding / L"original.cache") && f.Present(replacement, f.holding / L"replacement.cache"), "Never follow leaf reparse; reject directory open or wrong identity and preserve targets");
    }
    {
        Fixture f; const auto i = f.Create();
        const auto r = f.Execute(i, {}, [&](HANDLE)
        {
            auto attributes = Open(f.Get(i).path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
            FILE_BASIC_INFO basic{}; basic.FileAttributes = FILE_ATTRIBUTE_HIDDEN;
            Check(attributes && SetFileInformationByHandle(attributes.get(), FileBasicInfo, &basic, sizeof(basic)), "Metadata-only access can bypass ordinary write-sharing protection");
        });
        Print("metadata_change_before_final_review", r);
        Check(!r.called && r.reason == Reason::Attributes && f.Present(i), "Final same-handle review rejects observed attribute change");
    }
    {
        Fixture f; const auto i = f.Create(); std::stop_source stop; stop.request_stop();
        auto r = f.Execute(i, stop.get_token()); Print("cancel_before_acquisition", r);
        Check(!r.called && r.state == State::Cancelled && f.Present(i), "Early cancel preserves file");
        std::stop_source late; r = f.Execute(i, late.get_token(), [&](HANDLE) { late.request_stop(); }); Print("cancel_at_commit_gate", r);
        Check(!r.called && r.state == State::Cancelled && f.Present(i), "Final gate cancellation preserves file");
    }
    {
        Fixture f; const auto i = f.Create(), next = f.Create(); std::stop_source stop;
        const auto r = f.Execute(i, stop.get_token(), {}, true); Print("pending_until_last_handle_closed", r);
        Snapshot s;
        Check(r.state == State::MarkedPending && r.pending && Read(f.Get(i).retained.get(), s) && SameId(s, f.Get(i).original) && s.standard.DeletePending, "Open receipt delays final disposition");
        stop.request_stop(); const auto remaining = f.Execute(next, stop.get_token());
        Check(r.state == State::MarkedPending && remaining.state == State::Cancelled && !remaining.called && f.Present(next), "Post-commit cancellation cannot relabel committed item, remaining item preserved");
        f.CloseReceipt(i); SetLastError(0);
        Check(GetFileAttributesW(f.Get(i).path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND, "Last close removes name; no recovery promise");
    }
    {
        Fixture f; const auto a = f.Create(), b = f.Create(), c = f.Create();
        Check(SetFileAttributesW(f.Get(b).path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "Partial rejection fixture");
        const std::vector<Result> records{ f.Execute(a), f.Execute(b), f.Execute(c) };
        unsigned done = 0, refused = 0;
        for (const auto& r : records) { if (r.state == State::NameAbsent) ++done; if (r.state == State::Rejected && !r.called) ++refused; }
        Check(records.size() == 3 && done == 2 && refused == 1 && f.Present(b), "Partial results account for every item, refused file intact");
        Print("partial_two_completed_one_rejected", records[1]);
    }
    {
        Fixture f; const auto a = f.Create(), b = f.Create(), c = f.Create(); std::stop_source stop;
        const auto first = f.Execute(a, stop.get_token()); stop.request_stop();
        const auto second = f.Execute(b, stop.get_token()), third = f.Execute(c, stop.get_token());
        Check(first.state == State::NameAbsent && second.state == State::Cancelled && third.state == State::Cancelled &&
            !second.called && !third.called && f.Present(b) && f.Present(c), "Mid-batch cancellation accounts for completed and remaining items");
        Print("batch_one_completed_two_cancelled", third);
    }
    std::cout << "{\"checks\":" << checks << ",\"cases\":" << cases << ",\"failures\":0,\"readyForRealFiles\":false,"
        "\"irreversible\":true,\"actualReleasedBytes\":0,\"spaceMeasured\":false,\"processCrashTested\":false,\"automaticFallback\":false}\n";
}
int wmain(int argc, wchar_t**)
{
    if (argc != 1) { std::cerr << "No external path or command-line input accepted\n"; return 2; }
    try { Run(); return 0; }
    catch (const std::exception& error) { std::cerr << "Check " << checks << " failed: " << error.what() << '\n'; return 1; }
}
