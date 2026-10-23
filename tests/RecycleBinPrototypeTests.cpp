// No command-line path, ACL mutation, elevation, permanent deletion or fixture cleanup.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RecycleBinPrototype.h"
#include <winioctl.h>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cstring>
namespace fs = std::filesystem;
using namespace RecycleExperiment;
static unsigned checks = 0;
static void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
static size_t Index(const Cleanup::CleanupPlan& plan, const fs::path& path)
{
    for (size_t i = 0; i < plan.Items().size(); ++i)
        if (_wcsicmp(plan.Items()[i].candidate.path.c_str(), path.c_str()) == 0) return i;
    throw std::runtime_error("Owned receipt missing from plan");
}
static void Print(const char* name, const Record& record)
{
    std::cout << "{\"case\":\"" << name << "\",\"state\":" << int(record.state) << ",\"reason\":" << int(record.reason)
        << ",\"planReason\":" << int(record.planReason) << ",\"error\":" << record.error
        << ",\"queued\":" << record.queued << ",\"performed\":" << record.performed << ",\"itemResult\":" << record.itemResult
        << ",\"abortedQuery\":" << record.abortedQuery << ",\"pre\":" << record.pre << ",\"post\":" << record.post
        << ",\"aborted\":" << record.aborted << ",\"recycleItem\":" << record.recycleItem
        << ",\"recycledIdentityMatches\":" << record.recycledIdentityMatches
        << ",\"recycledReplacementMatches\":" << record.recycledReplacementMatches << "}\n";
}
class RestrictedScope
{
public:
    RestrictedScope()
    {
        HANDLE process = nullptr, restricted = nullptr;
        Check(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, &process) != FALSE,
            "Open own token for restricted access test");
        BYTE sid[SECURITY_MAX_SID_SIZE]{}; DWORD size = sizeof(sid);
        bool ok = CreateWellKnownSid(WinRestrictedCodeSid, nullptr, sid, &size) != FALSE;
        SID_AND_ATTRIBUTES restriction{ sid, 0 };
        if (ok) ok = CreateRestrictedToken(process, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1, &restriction, &restricted) != FALSE;
        CloseHandle(process);
        if (ok) ok = ImpersonateLoggedOnUser(restricted) != FALSE;
        if (restricted) CloseHandle(restricted);
        Check(ok, "Impersonate own restricted token without modifying ACL or elevating");
        active = true;
    }
    ~RestrictedScope() { if (active && !RevertToSelf()) std::terminate(); }
private:
    bool active = false;
};
static void TestPinnedAncestors(Prototype& prototype)
{
    const auto directory = prototype.Path(0).parent_path();
    Check(!MoveFileExW(directory.c_str(), (directory.wstring() + L".moved").c_str(), 0) && GetLastError() == ERROR_SHARING_VIOLATION,
        "Pinned ancestor refuses rename");
    HANDLE writer = CreateFileW(directory.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const DWORD error = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
    Check(writer == INVALID_HANDLE_VALUE && error == ERROR_SHARING_VIOLATION,
        "Pinned ancestor refuses handle needed to install a reparse point");
}
static void TestReparseAncestor(Prototype& prototype)
{
    const auto link = prototype.Root() / L"reparse-probe";
    Check(CreateDirectoryW(link.c_str(), nullptr) != FALSE, "Create owned reparse probe directory");
    const std::wstring target = L"\\??\\" + prototype.Path(0).parent_path().wstring();
    struct Data { DWORD tag; WORD length, reserved, subOffset, subLength, printOffset, printLength; wchar_t paths[32768]; } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT; data.subLength = WORD(target.size() * sizeof(wchar_t));
    data.printOffset = WORD(data.subLength + sizeof(wchar_t)); data.length = WORD(8 + data.printOffset + sizeof(wchar_t));
    memcpy(data.paths, target.c_str(), (target.size() + 1) * sizeof(wchar_t));
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    DWORD returned = 0;
    const bool made = handle != INVALID_HANDLE_VALUE && DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        DWORD(data.length) + 8, nullptr, 0, &returned, nullptr);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    Check(made, "Install junction only in owned probe directory");
    wchar_t old[32768]{}; const DWORD length = GetEnvironmentVariableW(L"TMP", old, 32768);
    Check(length < 32768, "Save process TMP override");
    Check(SetEnvironmentVariableW(L"TMP", link.c_str()) != FALSE, "Point process TMP at owned probe");
    bool refused = false;
    try { Prototype unsafe; } catch (const std::exception&) { refused = true; }
    Check(SetEnvironmentVariableW(L"TMP", length ? old : nullptr) != FALSE, "Restore process TMP override");
    Check(refused, "Constructor rejects a pre-existing reparse ancestor before creating files");
}
static void Run()
{
    Prototype prototype;
    for (unsigned i = 0; i < 10; ++i) Check(prototype.Create() == i, "Fresh owned receipt created");
    auto plan = prototype.Plan();
    Check(plan.Reason() == Cleanup::PlanReason::None && plan.Items().size() == 10, "Existing scanner/plan accepts only owned synthetic files");
    auto review = prototype.Evaluate(plan);
    Check(!review.readyForRealFiles && review.actualReleasedBytes == 0 && review.records.size() == 10 && review.blocked == 10,
        "Plan backend fails closed despite valid synthetic observations");
    for (size_t i = 0; i < review.records.size(); ++i)
        Check(review.records[i].itemIndex == i && review.records[i].reason == Reason::BindingUnavailable && !review.records[i].pre,
            "Identity binding unavailable is explicit and never enters Shell");
    TestPinnedAncestors(prototype);
    TestReparseAncestor(prototype);
    std::stop_source cancelled; cancelled.request_stop();
    review = prototype.Evaluate(plan, cancelled.get_token());
    Check(review.cancelled == 10 && review.blocked == 0 && review.records.size() == 10, "Cancelled plan accounts for every item");
    const auto beforeCancel = prototype.ShellControl(plan, Index(plan, prototype.Path(0)), Control::Normal, cancelled.get_token());
    Print("cancel_before_queue", beforeCancel);
    Check(beforeCancel.state == State::Cancelled && !beforeCancel.pre && prototype.IsOriginal(0), "Cancellation makes no Shell call");
    const auto atPre = prototype.ShellControl(plan, Index(plan, prototype.Path(0)), Control::CancelAtPre);
    Print("cancel_at_pre", atPre);
    Check(atPre.state == State::Cancelled && atPre.pre && prototype.IsOriginal(0), "Callback cancellation preserves owned file");
    // Holding a non-delete-sharing verified identity makes the separate Shell operation fail.
    HANDLE guard = CreateFileW(prototype.Path(1).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Check(guard != INVALID_HANDLE_VALUE, "Hold verified file without delete sharing");
    const auto locked = prototype.ShellControl(plan, Index(plan, prototype.Path(1)));
    Print("identity_lock", locked);
    CloseHandle(guard);
    Check(locked.state == State::Failed && locked.error == ERROR_SHARING_VIOLATION && prototype.IsOriginal(1),
        "Do not release the strong guard and retry to make recycling succeed");
    Record denied;
    { RestrictedScope restricted; denied = prototype.ShellControl(plan, Index(plan, prototype.Path(2))); }
    Print("restricted_permission", denied);
    Check(denied.state != State::Recycled && denied.error == ERROR_ACCESS_DENIED && prototype.IsOriginal(2),
        "Real access denial without ACL writes leaves file intact");
    const auto normal = prototype.ShellControl(plan, Index(plan, prototype.Path(3)));
    Print("normal_shell_control", normal);
    Check(normal.state == State::Recycled && normal.post && normal.recycleItem && normal.recycledIdentityMatches &&
        !fs::exists(prototype.Path(3)), "Normal CONTROL verifies actual item result and recycled identity");
    // Refresh the plan after the first real operation; no replay of an already recycled plan.
    plan = prototype.Plan();
    const auto changed = prototype.ShellControl(plan, Index(plan, prototype.Path(4)), Control::ReplaceBeforePre);
    Print("replace_before_pre", changed);
    Check(changed.state == State::Blocked && changed.reason == Reason::Changed && changed.pre &&
        fs::exists(prototype.Path(4)) && fs::exists(prototype.Root() / L"retained" / prototype.Path(4).filename()),
        "Path replacement before final callback is vetoed, both owned files survive");
    plan = prototype.Plan();
    const auto raced = prototype.ShellControl(plan, Index(plan, prototype.Path(6)), Control::ReplaceAfterPre);
    Print("replace_after_pre_CONTROL", raced);
    Check(raced.state == State::Recycled && raced.recycleItem && !raced.recycledIdentityMatches && raced.recycledReplacementMatches &&
        !fs::exists(prototype.Path(6)) && fs::exists(prototype.Root() / L"retained" / prototype.Path(6).filename()),
        "Counterexample: callback identity check does not atomically bind Shell disposition");
    // Three individual control results, including a genuine sharing failure between two successes.
    plan = prototype.Plan();
    std::vector<Record> partial;
    partial.push_back(prototype.ShellControl(plan, Index(plan, prototype.Path(8))));
    guard = CreateFileW(prototype.Path(9).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(guard != INVALID_HANDLE_VALUE, "Hold second partial-operation target");
    partial.push_back(prototype.ShellControl(plan, Index(plan, prototype.Path(9))));
    CloseHandle(guard);
    // A fresh plan is required by the test driver; this is an explicit separate control, not automatic retry.
    plan = prototype.Plan(); partial.push_back(prototype.ShellControl(plan, Index(plan, prototype.Path(0))));
    size_t recycled = 0, failed = 0;
    for (const auto& item : partial)
    { Print("partial_control", item); recycled += item.state == State::Recycled; failed += item.state == State::Failed; }
    Check(partial.size() == 3 && recycled == 2 && failed == 1 && prototype.IsOriginal(9), "Partial results and counts agree");
    // Foreign instance's genuine plan is still rejected before reading its candidate paths.
    Prototype foreign; foreign.Create(); const auto foreignPlan = foreign.Plan();
    review = prototype.Evaluate(foreignPlan);
    Check(review.records.size() == 1 && review.records[0].reason == Reason::NotOwned && foreign.IsOriginal(0),
        "Another session cannot grant ownership through a plan");
    Check(prototype.ShellControl(foreignPlan, 0).reason == Reason::NotOwned, "Shell rejects foreign receipt");
    std::cout << "{\"checks\":" << checks << ",\"failures\":0,\"readyForRealFiles\":false,\"permanentDeleteImplemented\":false,"
        "\"actualReleasedBytes\":0,\"partialRecycled\":" << recycled << ",\"partialFailed\":" << failed << "}\n";
}
int wmain(int argc, wchar_t**)
{
    if (argc != 1) { std::cerr << "No external path or command-line inputs accepted\n"; return 2; }
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 2;
    int exitCode = 0;
    try { Run(); }
    catch (const std::exception& error) { std::cerr << "Check " << checks << " failed: " << error.what() << '\n'; exitCode = 1; }
    CoUninitialize(); return exitCode;
}
