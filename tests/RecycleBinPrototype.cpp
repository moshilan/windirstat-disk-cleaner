// Test-only Shell experiment; the plan backend fails closed on missing atomic identity binding.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RecycleBinPrototype.h"
#include <shobjidl.h>
#include <shellapi.h>
#include <atlbase.h>
#include <algorithm>
#include <stdexcept>
#include <functional>
#include <cstring>

namespace fs = std::filesystem;
namespace RecycleExperiment
{
    namespace
    {
        using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
        Handle Open(const fs::path& path, DWORD access, DWORD share, DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT)
        {
            const auto name = path.wstring().starts_with(L"\\\\?\\") ? path.wstring() : L"\\\\?\\" + path.wstring();
            auto value = CreateFileW(name.c_str(), access, share, nullptr, OPEN_EXISTING, flags, nullptr);
            return Handle(value == INVALID_HANDLE_VALUE ? nullptr : value, CloseHandle);
        }
        bool Identity(HANDLE h, const Cleanup::Candidate& candidate)
        {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(h, &info)) return false;
            std::array<std::uint8_t, 16> id{};
            const auto index = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
            memcpy(id.data(), &index, sizeof(index));
            return candidate.volumeSerial == info.dwVolumeSerialNumber && candidate.fileId == id &&
                info.nNumberOfLinks == 1 && (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0;
        }
        bool FinalPath(HANDLE h, const fs::path& expected)
        {
            std::wstring value(32768, L'\0');
            const auto length = GetFinalPathNameByHandleW(h, value.data(), DWORD(value.size()), FILE_NAME_NORMALIZED);
            if (!length || length >= value.size()) return false;
            value.resize(length);
            return _wcsicmp(value.c_str(), (L"\\\\?\\" + expected.wstring()).c_str()) == 0;
        }
        // Open from the drive down; each previously checked component stays pinned. Denying
        // write/delete sharing prevents ordinary rename and reparse writes to those objects.
        std::vector<Handle> PinAncestors(const fs::path& directory)
        {
            std::vector<Handle> pins;
            auto path = directory.root_path();
            if (GetDriveTypeW(path.c_str()) != DRIVE_FIXED) throw std::runtime_error("Fixed local drive required");
            wchar_t fileSystem[32]{};
            if (!GetVolumeInformationW(path.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fileSystem, 32) ||
                _wcsicmp(fileSystem, L"NTFS") != 0) throw std::runtime_error("NTFS required");
            const auto pin = [&](const fs::path& component)
            {
                auto handle = Open(component, GENERIC_READ, FILE_SHARE_READ,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS);
                FILE_ATTRIBUTE_TAG_INFO tag{};
                if (!handle || !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
                    (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    !FinalPath(handle.get(), component)) throw std::runtime_error("Cannot bind safe ancestor");
                pins.push_back(std::move(handle));
            };
            pin(path);
            for (const auto& part : directory.relative_path()) { path /= part; pin(path); }
            return pins;
        }
        class Sink : public IFileOperationProgressSink
        {
        public:
            Record& record;
            std::function<HRESULT()> before;
            explicit Sink(Record& value) : record(value) {}
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override
            {
                if (!out) return E_POINTER;
                *out = nullptr;
                if (iid != IID_IUnknown && iid != __uuidof(IFileOperationProgressSink)) return E_NOINTERFACE;
                *out = static_cast<IFileOperationProgressSink*>(this); AddRef(); return S_OK;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
            ULONG STDMETHODCALLTYPE Release() override { const auto n = --references; if (!n) delete this; return n; }
            HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
            HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD flags, IShellItem*) override
            {
                record.pre = true;
                if (!(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)) return E_ABORT;
                try { return before(); } catch (...) { return E_ABORT; }
            }
            HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT result, IShellItem* recycled) override
            {
                record.post = true; record.itemResult = result; record.recycleItem = recycled != nullptr;
                if (recycled) recycled->QueryInterface(IID_PPV_ARGS(&recycledItem));
                return FAILED(result) || !recycled ? E_ABORT : S_OK;
            }
            HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return E_ABORT; }
            HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ABORT; }
            HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ABORT; }
            HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ABORT; }
            HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return E_ABORT; }
            HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return E_ABORT; }
            HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return E_ABORT; }
            HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return E_ABORT; }
            HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
            HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
            HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
            HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }
            CComPtr<IShellItem> recycledItem;
        private:
            ULONG references = 1;
        };
    }
    struct Prototype::Impl
    {
        fs::path root, temp;
        Cleanup::ScanOptions options;
        struct Receipt { fs::path path; Handle retained{ nullptr, CloseHandle }; Cleanup::Candidate identity; };
        std::vector<Receipt> receipts;
        std::vector<Handle> pins;
        size_t Find(const Cleanup::Candidate& candidate) const
        {
            for (size_t i = 0; i < receipts.size(); ++i)
                if (_wcsicmp(candidate.path.c_str(), receipts[i].path.c_str()) == 0 &&
                    candidate.volumeSerial == receipts[i].identity.volumeSerial && candidate.fileId == receipts[i].identity.fileId) return i;
            return SIZE_MAX;
        }
    };
    Prototype::Prototype() : m(std::make_unique<Impl>())
    {
        wchar_t temp[32768]{}; GUID guid{}; wchar_t name[40]{};
        const DWORD length = GetTempPathW(32768, temp);
        if (!length || length >= 32768 || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, name, 40))
            throw std::runtime_error("Cannot generate fixture root");
        std::wstring normalized, error;
        if (!Cleanup::NormalizeDirectory(temp, normalized, error)) throw std::runtime_error("Unsafe temporary directory");
        const fs::path parent(normalized);
        auto parentPins = PinAncestors(parent);
        m->root = parent / (L"WinDirStatRecycleExperiment-" + std::wstring(name));
        if (!CreateDirectoryW(m->root.c_str(), nullptr)) throw std::runtime_error("Fixture must be newly created");
        auto rootPins = PinAncestors(m->root);
        if (!CreateDirectoryW((m->root / L"LocalAppData").c_str(), nullptr)) throw std::runtime_error("Create anchor failed");
        auto anchorPins = PinAncestors(m->root / L"LocalAppData");
        m->temp = m->root / L"LocalAppData" / L"Temp";
        if (!CreateDirectoryW(m->temp.c_str(), nullptr)) throw std::runtime_error("Create fixture failed");
        m->pins = PinAncestors(m->temp);
        m->options.localAppData = (m->root / L"LocalAppData").wstring();
        m->options.enabled.fill(false); m->options.enabled[0] = true;
    }
    Prototype::~Prototype() = default; // Preserve all remaining files. Never clean up by deleting.
    size_t Prototype::Create()
    {
        Impl::Receipt receipt;
        receipt.path = m->temp / (std::to_wstring(m->receipts.size()) + L".tmp");
        Handle writer(CreateFileW(receipt.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr), CloseHandle);
        if (writer.get() == INVALID_HANDLE_VALUE) { writer.release(); throw std::runtime_error("CREATE_NEW failed"); }
        FILETIME clock{}; GetSystemTimeAsFileTime(&clock);
        const auto old = ((std::uint64_t(clock.dwHighDateTime) << 32) | clock.dwLowDateTime) - 8ULL * 86400 * 10000000;
        const FILETIME age{ DWORD(old), DWORD(old >> 32) }; DWORD written = 0;
        const char payload[] = "only this process created this synthetic recycle payload";
        if (!WriteFile(writer.get(), payload, sizeof(payload), &written, nullptr) || written != sizeof(payload) ||
            !SetFileTime(writer.get(), &age, nullptr, &age)) throw std::runtime_error("Fixture creation failed");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(writer.get(), &info)) throw std::runtime_error("Creating handle identity failed");
        receipt.identity.volumeSerial = info.dwVolumeSerialNumber;
        const auto index = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        memcpy(receipt.identity.fileId.data(), &index, sizeof(index));
        // Derive ownership from CREATE_NEW itself, never by closing and reopening its name.
        HANDLE retained = ReOpenFile(writer.get(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
        if (retained == INVALID_HANDLE_VALUE) throw std::runtime_error("Retain creating object failed");
        receipt.retained.reset(retained);
        if (!Identity(receipt.retained.get(), receipt.identity)) throw std::runtime_error("Receipt identity mismatch");
        writer.reset();
        m->receipts.push_back(std::move(receipt)); return m->receipts.size() - 1;
    }
    const fs::path& Prototype::Path(size_t i) const { return m->receipts.at(i).path; }
    const fs::path& Prototype::Root() const { return m->root; }
    bool Prototype::IsOriginal(size_t i) const
    {
        auto handle = Open(Path(i), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        return handle && Identity(handle.get(), m->receipts.at(i).identity);
    }
    Cleanup::CleanupPlan Prototype::Plan() const
    {
        const auto preview = Cleanup::Scan(m->options);
        std::vector<bool> selected;
        for (const auto& candidate : preview.candidates) selected.push_back(m->Find(candidate) != SIZE_MAX);
        return Cleanup::BuildPlan(preview, selected, m->options);
    }
    Review Prototype::Evaluate(const Cleanup::CleanupPlan& plan, std::stop_token stop) const
    {
        Review result; result.planId = plan.Id();
        if (std::ranges::any_of(plan.Items(), [&](const auto& item) { return m->Find(item.candidate) == SIZE_MAX; }))
        {
            for (size_t i = 0; i < plan.Items().size(); ++i)
            { Record item; item.itemIndex = i; item.reason = Reason::NotOwned; result.records.push_back(item); ++result.blocked; }
            return result;
        }
        // Do not scan caller-supplied roots. This instance's options and receipts are the only inputs.
        const auto simulation = Cleanup::SimulatePlan(plan, m->options, stop);
        for (const auto& observation : simulation.records)
        {
            Record item; item.itemIndex = observation.itemIndex; item.planReason = observation.reason; item.error = observation.systemError;
            const auto& candidate = plan.Items()[item.itemIndex];
            if (observation.state == Cleanup::SimulationState::Cancelled)
            { item.state = State::Cancelled; item.reason = Reason::Cancelled; ++result.cancelled; }
            else
            {
                ++result.blocked;
                if (m->Find(candidate.candidate) == SIZE_MAX) item.reason = Reason::NotOwned;
                else if (candidate.disposition != Cleanup::Disposition::RecycleBin || observation.reason != Cleanup::PlanReason::None)
                    item.reason = Reason::PlanRejected;
                // Otherwise BindingUnavailable: never call Shell from the plan backend.
            }
            result.records.push_back(item);
        }
        return result;
    }
    Record Prototype::ShellControl(const Cleanup::CleanupPlan& plan, size_t itemIndex, Control control,
        size_t replacementReceipt, std::stop_token stop)
    {
        Record result; result.itemIndex = itemIndex;
        if (stop.stop_requested()) { result.state = State::Cancelled; result.reason = Reason::Cancelled; return result; }
        if (itemIndex >= plan.Items().size()) { result.reason = Reason::PlanRejected; return result; }
        const auto& item = plan.Items()[itemIndex]; const size_t receipt = m->Find(item.candidate);
        if (receipt == SIZE_MAX) { result.reason = Reason::NotOwned; return result; }
        const auto review = Evaluate(plan, stop);
        const auto& gate = review.records.at(itemIndex);
        if (gate.reason != Reason::BindingUnavailable) return gate;
        // No retry with weaker sharing: this CONTROL deliberately permits delete sharing initially.
        auto observed = Open(Path(receipt), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        if (!observed) { result.state = State::Failed; result.reason = Reason::Access; result.error = GetLastError(); return result; }
        if (!Identity(observed.get(), item.candidate) || !FinalPath(observed.get(), Path(receipt)))
        { result.reason = Reason::Changed; return result; }
        const bool replacing = control == Control::ReplaceBeforePre || control == Control::ReplaceAfterPre;
        if (replacing && (replacementReceipt >= m->receipts.size() || replacementReceipt == receipt || !IsOriginal(replacementReceipt)))
        { result.reason = Reason::NotOwned; return result; }
        const auto replace = [&]()
        {
            const auto saved = Path(receipt).wstring() + L".retained";
            // Both names and both objects are generated/owned by this instance; no overwrite allowed.
            return MoveFileExW(Path(receipt).c_str(), saved.c_str(), 0) &&
                MoveFileExW(Path(replacementReceipt).c_str(), Path(receipt).c_str(), 0);
        };
        CComPtr<IFileOperation> operation; CComPtr<IShellItem> source;
        HRESULT hr = operation.CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER);
        constexpr DWORD flags = FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO | FOF_SILENT | FOF_NOERRORUI |
            FOFX_EARLYFAILURE | FOFX_NOCOPYHOOKS | FOF_NO_CONNECTED_ELEMENTS | FOF_NOCONFIRMATION;
        if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(flags);
        if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(Path(receipt).c_str(), nullptr, IID_PPV_ARGS(&source));
        CComPtr<Sink> sink; sink.Attach(new Sink(result));
        sink->before = [&]() -> HRESULT
        {
            if (stop.stop_requested() || control == Control::CancelAtPre)
            { result.state = State::Cancelled; result.reason = Reason::Cancelled; return E_ABORT; }
            if (!IsOriginal(receipt)) { result.reason = Reason::Changed; return E_ABORT; }
            if (control == Control::ReplaceAfterPre && !replace()) return E_ABORT;
            return S_OK; // Explicit race experiment: no atomic expected-identity condition follows this callback.
        };
        if (SUCCEEDED(hr)) hr = result.queued = operation->DeleteItem(source, sink);
        if (SUCCEEDED(hr) && control == Control::ReplaceBeforePre && !replace()) hr = E_ABORT;
        if (SUCCEEDED(hr))
        {
            result.performed = operation->PerformOperations(); BOOL aborted = TRUE;
            result.abortedQuery = operation->GetAnyOperationsAborted(&aborted); result.aborted = aborted != FALSE;
        }
        else result.performed = hr;
        if (result.state == State::Cancelled || result.reason == Reason::Changed) return result;
        result.reason = Reason::Shell;
        if (result.post && SUCCEEDED(result.itemResult) && result.recycleItem)
        {
            PWSTR path = nullptr;
            if (sink->recycledItem && SUCCEEDED(sink->recycledItem->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                auto recycled = Open(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
                result.recycledIdentityMatches = recycled && Identity(recycled.get(), item.candidate);
                CoTaskMemFree(path);
            }
            result.state = SUCCEEDED(result.performed) && SUCCEEDED(result.abortedQuery) && !result.aborted ? State::Recycled : State::Unknown;
            result.reason = Reason::None;
        }
        else result.state = result.post && SUCCEEDED(result.itemResult) ? State::Unknown : State::Failed;
        return result;
    }
}
