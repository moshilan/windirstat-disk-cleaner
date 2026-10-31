// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include "BleachBit.h"
#include "CleanupPreview.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace Cleanup::BleachBit
{
    namespace
    {
        constexpr wchar_t Arguments[] = L"--no-uac --no-load-cleaners --no-check-online-updates --no-first-start ";
        constexpr size_t OutputLimit = 1024 * 1024;
        struct Handle
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            Handle() = default;
            explicit Handle(HANDLE h) : value(h) {}
            ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
            Handle(Handle&& h) noexcept : value(std::exchange(h.value, INVALID_HANDLE_VALUE)) {}
            Handle& operator=(Handle&& h) noexcept
            {
                if (this != &h)
                {
                    if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
                    value = std::exchange(h.value, INVALID_HANDLE_VALUE);
                }
                return *this;
            }
            explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
        };
        struct Less { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
        using Environment = std::map<std::wstring, std::wstring, Less>;
        bool Equal(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
        std::wstring Error(const wchar_t* operation) { return std::wstring(operation) + L" (Win32 " + std::to_wstring(GetLastError()) + L")"; }
        Environment Snapshot()
        {
            Environment result;
            const auto block = GetEnvironmentStringsW();
            if (!block) return result;
            for (const wchar_t* row = block; *row; row += wcslen(row) + 1)
                if (const auto split = wcschr(row + 1, L'=')) result.emplace(std::wstring(row, split), split + 1);
            FreeEnvironmentStringsW(block);
            return result;
        }
        std::wstring Value(const Environment& env, const wchar_t* name)
        {
            const auto found = env.find(name);
            return found == env.end() ? L"" : found->second;
        }
        std::wstring Known(const KNOWNFOLDERID& id)
        {
            PWSTR buffer = nullptr;
            if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &buffer))) return {};
            std::wstring result(buffer); CoTaskMemFree(buffer); return result;
        }
        bool Unprivileged(std::wstring& diagnostic)
        {
            HANDLE raw = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) { diagnostic = Error(L"Read process token"); return false; }
            Handle token(raw);
            TOKEN_ELEVATION elevated{}; DWORD bytes = 0;
            if (!GetTokenInformation(token.value, TokenElevation, &elevated, sizeof(elevated), &bytes))
            { diagnostic = Error(L"Read process elevation"); return false; }
            std::array<BYTE, SECURITY_MAX_SID_SIZE> sid{}; DWORD sidBytes = static_cast<DWORD>(sid.size());
            BOOL member = FALSE;
            if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid.data(), &sidBytes) ||
                !CheckTokenMembership(nullptr, sid.data(), &member))
            { diagnostic = Error(L"Check enabled administrator group"); return false; }
            if (elevated.TokenIsElevated || member)
            {
                diagnostic = L"BleachBit cleaning is disabled in an elevated or administrator-enabled process. Restart WinDirStat with ordinary user privileges; no elevation or reboot deletion is requested.";
                return false;
            }
            return true;
        }
        struct Identity
        {
            DWORD volume = 0, high = 0, low = 0;
            bool operator==(const Identity&) const = default;
        };
        bool IdentityOf(HANDLE file, Identity& id)
        {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(file, &info)) return false;
            id = { info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow }; return true;
        }
        bool FinalPath(HANDLE handle, std::wstring& path)
        {
            const DWORD length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (!length) return false;
            path.assign(length, L'\0');
            const DWORD copied = GetFinalPathNameByHandleW(handle, path.data(), length, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (!copied || copied >= length) return false;
            path.resize(copied);
            if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
            return true;
        }
        struct Location
        {
            std::wstring path;
            Identity identity;
            Handle handle;
            bool metadataOnly = false;
        };
        bool OpenLocation(const std::wstring& input, bool directory, Location& result, std::wstring& diagnostic, bool metadataOnly = false)
        {
            std::wstring path;
            if (!NormalizeDirectory(input, path, diagnostic) || !HasSafeAncestors(path))
            { diagnostic = L"An engine, scope or configuration path is unsafe or cannot be verified: " + input; return false; }
            Handle h(CreateFileW((L"\\\\?\\" + path).c_str(), metadataOnly ? FILE_READ_ATTRIBUTES : GENERIC_READ, FILE_SHARE_READ | (directory ? FILE_SHARE_WRITE : 0),
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
            FILE_ATTRIBUTE_TAG_INFO attributes{}; std::wstring final;
            if (!h || !GetFileInformationByHandleEx(h.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
                !FinalPath(h.value, final) || !IdentityOf(h.value, result.identity))
            { diagnostic = Error(L"Open and identify engine scope"); return false; }
            if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory || !Equal(path, final))
            { diagnostic = L"An engine or scope path resolves to an unexpected object: " + input; return false; }
            wchar_t root[32768]{}, filesystem[32]{};
            if (!GetVolumePathNameW(path.c_str(), root, static_cast<DWORD>(std::size(root))) || GetDriveTypeW(root) != DRIVE_FIXED ||
                !GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, filesystem, static_cast<DWORD>(std::size(filesystem))) || !Equal(filesystem, L"NTFS"))
            { diagnostic = L"Only verified local fixed NTFS locations are supported: " + input; return false; }
            result.path = std::move(path); result.handle = std::move(h); result.metadataOnly = metadataOnly; return true;
        }
        bool SameLocation(const Location& expected, bool directory, std::wstring& diagnostic)
        {
            Location current;
            if (!OpenLocation(expected.path, directory, current, diagnostic, expected.metadataOnly)) return false;
            if (current.identity != expected.identity) { diagnostic = L"An engine or scope identity changed after preview: " + expected.path; return false; }
            return true;
        }
        bool CheckExclusions(const ExclusionPolicy& policy, const std::wstring& root, std::wstring& diagnostic)
        {
            std::vector<std::wstring> directories;
            if (!policy.available || !ParseExclusions(policy.text, directories, diagnostic))
            { diagnostic = L"Exclusion rules are unavailable or invalid. No cleaning is allowed. " + diagnostic; return false; }
            for (const auto& path : directories)
            {
                if (IsWithin(root, path) || IsWithin(path, root))
                { diagnostic = L"The entire BleachBit temporary-files category overlaps an excluded directory: " + path + L". This category cannot be cleaned by overriding the confirmation."; return false; }
                Location excluded;
                if (!OpenLocation(path, true, excluded, diagnostic, true))
                { diagnostic = L"An excluded directory cannot be reliably resolved; cleaning is blocked: " + path; return false; }
                if (IsWithin(root, excluded.path) || IsWithin(excluded.path, root))
                { diagnostic = L"A resolved exclusion overlaps the BleachBit temporary-files category: " + path; return false; }
            }
            return true;
        }
        std::wstring Decode(const std::string& bytes)
        {
            if (bytes.empty()) return {};
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (!size) return {};
            std::wstring result(size, L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), result.data(), size); return result;
        }
        struct ProcessResult
        {
            bool started = false, cancelled = false, timedOut = false, truncated = false;
            DWORD exitCode = ERROR_PROCESS_ABORTED;
            std::wstring output, diagnostic;
        };
        ProcessResult Run(const std::wstring& tool, const std::wstring& arguments, const Environment& env,
            const std::wstring& cwd, const std::stop_token stop)
        {
            ProcessResult result;
            if (stop.stop_requested()) { result.cancelled = true; return result; }
            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE read = nullptr, write = nullptr;
            if (!CreatePipe(&read, &write, &security, 0)) { result.diagnostic = Error(L"Create command pipe"); return result; }
            Handle reader(read), writer(write);
            SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
            Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
            Handle job(CreateJobObjectW(nullptr, nullptr));
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!input || !job || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            { result.diagnostic = Error(L"Create command cancellation job"); return result; }
            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) { result.diagnostic = Error(L"Initialize process handles"); return result; }
            const std::array inherited{ write, input.value };
            if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr))
            { DeleteProcThreadAttributeList(attributes); result.diagnostic = Error(L"Restrict process handles"); return result; }
            STARTUPINFOEXW start{};
            start.StartupInfo.cb = sizeof(start); start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            start.StartupInfo.hStdInput = input.value; start.StartupInfo.hStdOutput = start.StartupInfo.hStdError = write;
            start.lpAttributeList = attributes;
            std::vector<wchar_t> block;
            for (const auto& [key, value] : env)
            {
                const auto line = key + L"=" + value;
                block.insert(block.end(), line.begin(), line.end()); block.push_back(0);
            }
            block.push_back(0);
            auto command = L"\"" + tool + L"\" " + arguments;
            PROCESS_INFORMATION process{};
            const bool created = CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                block.data(), cwd.c_str(), &start.StartupInfo, &process) != FALSE;
            DeleteProcThreadAttributeList(attributes);
            if (!created) { result.diagnostic = Error(L"Start BleachBit command"); return result; }
            Handle child(process.hProcess), thread(process.hThread);
            if (!AssignProcessToJobObject(job.value, process.hProcess))
            {
                result.diagnostic = Error(L"Bind BleachBit to cancellation job");
                TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED); WaitForSingleObject(process.hProcess, 5000); return result;
            }
            if (stop.stop_requested() || ResumeThread(process.hThread) == static_cast<DWORD>(-1))
            {
                result.cancelled = stop.stop_requested(); result.diagnostic = L"The command was not resumed.";
                TerminateJobObject(job.value, ERROR_CANCELLED); WaitForSingleObject(child.value, 5000); return result;
            }
            result.started = true;
            CloseHandle(std::exchange(writer.value, INVALID_HANDLE_VALUE));
            std::string output; const auto began = GetTickCount64();
            auto drain = [&]
            {
                DWORD available = 0; std::array<char, 4096> buffer{};
                while (PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) && available)
                {
                    DWORD received = 0;
                    if (!ReadFile(reader.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &received, nullptr) || !received) break;
                    if (received > OutputLimit - output.size()) result.truncated = true;
                    if (output.size() < OutputLimit) output.append(buffer.data(), std::min<size_t>(received, OutputLimit - output.size()));
                    if (result.truncated) break;
                }
            };
            while (WaitForSingleObject(child.value, 20) == WAIT_TIMEOUT)
            {
                drain();
                if (stop.stop_requested() || result.truncated || GetTickCount64() - began > 120000)
                {
                    result.cancelled = stop.stop_requested(); result.timedOut = !result.cancelled && !result.truncated;
                    TerminateJobObject(job.value, ERROR_CANCELLED); WaitForSingleObject(child.value, 5000); break;
                }
            }
            drain(); GetExitCodeProcess(child.value, &result.exitCode); result.output = Decode(output);
            if (result.output.empty() && !output.empty()) result.diagnostic = L"The command output was not valid UTF-8.";
            if (result.cancelled || result.timedOut || result.truncated)
                result.diagnostic = L"BleachBit was stopped, timed out or exceeded the output limit. A started cleanup may be partially complete; no retry or fallback was performed.";
            return result;
        }
        struct Summary { std::optional<std::uint64_t> files; std::uint64_t errors = 0; bool valid = false, hasErrors = false; };
        bool Number(const std::wstring& text, std::uint64_t& number)
        {
            if (text.empty()) return false;
            number = 0;
            for (const auto c : text)
            {
                if (c < L'0' || c > L'9' || number > (std::numeric_limits<std::uint64_t>::max() - (c - L'0')) / 10) return false;
                number = number * 10 + (c - L'0');
            }
            return true;
        }
        Summary ReadSummary(const ProcessResult& process, bool clean)
        {
            Summary result;
            if (!process.started || process.exitCode || process.cancelled || process.timedOut || process.truncated || !process.diagnostic.empty()) return result;
            std::wistringstream lines(process.output); std::wstring line, last;
            bool space = false, special = false, suspicious = false;
            const std::wstring filesPrefix = clean ? L"Files deleted: " : L"Files to be deleted: ";
            const std::wstring spacePrefix = clean ? L"Disk space recovered: " : L"Disk space to be recovered: ";
            while (std::getline(lines, line))
            {
                if (!line.empty() && line.back() == L'\r') line.pop_back();
                if (line.empty()) continue;
                last = line;
                if (line.starts_with(spacePrefix)) { if (space) suspicious = true; space = true; }
                else if (line.starts_with(filesPrefix))
                {
                    std::uint64_t number = 0;
                    if (result.files || !Number(line.substr(filesPrefix.size()), number)) suspicious = true;
                    else result.files = number;
                }
                else if (line.starts_with(L"Errors: "))
                {
                    if (result.hasErrors || !Number(line.substr(8), result.errors) || !result.errors) suspicious = true;
                    else result.hasErrors = true;
                }
                else if (line.starts_with(L"Special operations:")) special = true;
                else if (!line.starts_with(L"Delete ") && !line.starts_with(L"Skip ")) suspicious = true;
            }
            result.valid = space && result.files.has_value() && !special && !suspicious &&
                (last.starts_with(filesPrefix) || last.starts_with(L"Errors: "));
            return result;
        }
        Measurement Measure(const std::wstring& root)
        {
            Measurement result; ULARGE_INTEGER available{};
            if (GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr)) result.availableBytes = available.QuadPart;
            return result;
        }
        const std::string ConfigBase =
            "[bleachbit]\nversion=6.0.6\nauto_hide=True\nauto_detect_lang=False\nforced_language=en\n"
            "check_beta=False\ncheck_online_updates=False\ndark_mode=True\ndebug=False\ndelete_confirmation=True\n"
            "expert_mode=False\nexit_done=False\nfirst_start=False\nkde_shred_menu_option=False\nload_cleaners=False\n"
            "remember_geometry=True\nshred=False\nunits_iec=False\nwindow_maximized=False\nwindow_fullscreen=False\n"
            "font_check_completed=False\nupdate_winapp2=False\nuse_fontconfig_backend=False\nwin10_theme=False\n"
            "\n[hashpath]\n\n[list/shred_drives]\n\n[whitelist/paths]\n0_type=folder\n0_path=";
        bool EncodePath(const std::wstring& path, std::string& encoded)
        {
            if (std::ranges::any_of(path, [](wchar_t c) { return c < 32 || (c >= 127 && c <= 159) || c == 0x2028 || c == 0x2029; })) return false;
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) return false;
            encoded.assign(count, '\0');
            return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), encoded.data(), count, nullptr, nullptr) == count;
        }
        bool ReadConfig(HANDLE file, const std::string& expected)
        {
            LARGE_INTEGER length{};
            if (!GetFileSizeEx(file, &length) || length.QuadPart != static_cast<LONGLONG>(expected.size())) return false;
            LARGE_INTEGER start{};
            if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
            std::string bytes(expected.size(), '\0'); DWORD read = 0;
            return ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size() && bytes == expected;
        }
        std::wstring Discover(const std::wstring& selected)
        {
            if (!selected.empty()) return selected;
            for (const auto& parent : { Known(FOLDERID_ProgramFilesX86), Known(FOLDERID_ProgramFiles), Known(FOLDERID_LocalAppData) })
            {
                if (parent.empty()) continue;
                const auto tool = parent + L"\\BleachBit\\bleachbit_console.exe";
                const auto attributes = GetFileAttributesW(tool.c_str());
                if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return tool;
            }
            return {};
        }
    }

    struct Prepared
    {
        Location tool, user, system, control;
        Handle config;
        ExclusionPolicy exclusions;
        Environment original, environment;
        std::wstring fixture, windows, configPath;
        std::string expectedConfig;
        bool fixtureRequested = false;
        mutable std::atomic_bool consumed = false;
    };

    Preview Prepare(const ExclusionPolicy& exclusions, const std::wstring& absoluteTool, const std::stop_token stop)
    {
        Preview result;
        if (stop.stop_requested()) { result.state = State::Cancelled; return result; }
        if (!Unprivileged(result.diagnostic)) { result.state = State::Blocked; return result; }
        auto prepared = std::make_shared<Prepared>();
        prepared->original = Snapshot(); prepared->environment = prepared->original; prepared->exclusions = exclusions;
        if (prepared->original.empty()) { result.state = State::Blocked; result.diagnostic = L"The command environment is unavailable."; return result; }
        prepared->fixture = Value(prepared->original, L"WDS_CLEANUP_TEST_ROOT");
        prepared->fixtureRequested = prepared->original.contains(L"WDS_CLEANUP_TEST_ROOT");
        std::wstring local, controlBase, error;
        if (prepared->fixtureRequested)
        {
            if (!ResolveTestFixture(prepared->fixture, local, error))
            { result.state = State::Blocked; result.diagnostic = L"Invalid synthetic fixture; no real scope fallback. " + error; return result; }
            result.userTemp = local + L"\\Temp";
            prepared->windows = prepared->fixture + L"\\Windows";
            controlBase = prepared->fixture + L"\\Control";
        }
        else
        {
            local = Known(FOLDERID_LocalAppData); result.userTemp = Value(prepared->original, L"TEMP");
            std::wstring actualTemp, standardTemp;
            if (!NormalizeDirectory(result.userTemp, actualTemp, error) ||
                !NormalizeDirectory(local + L"\\Temp", standardTemp, error) || !Equal(actualTemp, standardTemp))
            { result.state = State::Blocked; result.diagnostic = L"Only the current user's standard LocalAppData\\Temp is supported. The actual TEMP environment points elsewhere or cannot be verified; no alternate directory was substituted."; return result; }
            wchar_t windows[32768]{};
            const auto count = GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)));
            if (!count || count >= std::size(windows)) { result.state = State::Blocked; result.diagnostic = Error(L"Resolve Windows directory"); return result; }
            prepared->windows = windows;
            std::wstring inheritedWindows;
            if (!NormalizeDirectory(Value(prepared->original, L"windir"), inheritedWindows, error) || !Equal(inheritedWindows, prepared->windows))
            { result.state = State::Blocked; result.diagnostic = L"The inherited Windows directory is unexpected. No category was started."; return result; }
            controlBase = local + L"\\WinDirStat\\BleachBitSessions";
        }
        result.systemTemp = prepared->windows + L"\\Temp";
        if (!OpenLocation(result.userTemp, true, prepared->user, result.diagnostic) ||
            !OpenLocation(result.systemTemp, true, prepared->system, result.diagnostic, true))
        { result.state = State::Blocked; return result; }
        result.userTemp = prepared->user.path; result.systemTemp = prepared->system.path;
        if (IsWithin(result.userTemp, result.systemTemp) || IsWithin(result.systemTemp, result.userTemp) ||
            IsWithin(controlBase, result.userTemp) || IsWithin(controlBase, result.systemTemp) ||
            !CheckExclusions(exclusions, result.userTemp, result.diagnostic))
        { result.state = State::Blocked; if (result.diagnostic.empty()) result.diagnostic = L"Temporary scopes or the private configuration overlap."; return result; }
        auto selected = absoluteTool;
        if (prepared->fixtureRequested && selected.empty()) selected = Value(prepared->original, L"WDS_BLEACHBIT_TEST_TOOL");
        result.tool = prepared->fixtureRequested ? selected : Discover(selected);
        if (result.tool.empty())
        { result.diagnostic = L"BleachBit 6.0.6 was not found. Install it yourself from bleachbit.org or select a trusted complete existing package; WinDirStat does not download or install it."; return result; }
        if (!Equal(std::filesystem::path(result.tool).filename().wstring(), L"bleachbit_console.exe"))
        { result.state = State::Blocked; result.diagnostic = L"Select bleachbit_console.exe from a trusted complete BleachBit 6.0.6 package."; return result; }
        if (!OpenLocation(result.tool, false, prepared->tool, result.diagnostic))
        { result.state = State::Blocked; return result; }
        result.tool = prepared->tool.path;
        if (IsWithin(result.tool, result.userTemp) || IsWithin(result.tool, result.systemTemp))
        { result.state = State::Blocked; result.diagnostic = L"The BleachBit package must be outside both cleanup roots."; return result; }
        GUID guid{}; wchar_t name[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, name, static_cast<int>(std::size(name))))
        { result.state = State::Blocked; result.diagnostic = L"Cannot create a private configuration identity."; return result; }
        std::error_code filesystemError;
        std::filesystem::create_directories(controlBase, filesystemError);
        const auto session = controlBase + L"\\" + name;
        if (filesystemError || !HasSafeAncestors(controlBase) || !CreateDirectoryW(session.c_str(), nullptr) ||
            !OpenLocation(session, true, prepared->control, result.diagnostic))
        { result.state = State::Blocked; if (result.diagnostic.empty()) result.diagnostic = L"Cannot create a safe private configuration directory."; return result; }
        prepared->configPath = prepared->control.path + L"\\bleachbit.ini";
        std::string keep;
        if (!EncodePath(result.systemTemp, keep))
        { result.state = State::Blocked; result.diagnostic = L"The protected Windows temporary path cannot be encoded safely in the engine configuration."; return result; }
        prepared->expectedConfig = ConfigBase + keep + "\n";
        prepared->config = Handle(CreateFileW(prepared->configPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        DWORD written = 0;
        if (!prepared->config || !WriteFile(prepared->config.value, prepared->expectedConfig.data(), static_cast<DWORD>(prepared->expectedConfig.size()), &written, nullptr) ||
            written != prepared->expectedConfig.size() || !FlushFileBuffers(prepared->config.value) || !ReadConfig(prepared->config.value, prepared->expectedConfig))
        { result.state = State::Blocked; result.diagnostic = Error(L"Create and verify private engine configuration"); return result; }
        auto& env = prepared->environment;
        for (auto item = env.begin(); item != env.end();)
        {
            if (_wcsnicmp(item->first.c_str(), L"BLEACHBIT_", 10) == 0 || _wcsnicmp(item->first.c_str(), L"PYTHON", 6) == 0) item = env.erase(item);
            else ++item;
        }
        env[L"TEMP"] = env[L"TMP"] = result.userTemp; env[L"windir"] = prepared->windows;
        env[L"BLEACHBIT_TEST_OPTIONS_DIR"] = prepared->control.path;
        env[L"PYTHONIOENCODING"] = L"utf-8";
        if (prepared->fixtureRequested) env[L"APPDATA"] = prepared->control.path;
        const auto version = Run(result.tool, std::wstring(Arguments) + L"--version", env, prepared->control.path, stop);
        if (version.cancelled) { result.state = State::Cancelled; result.output = version.output; result.diagnostic = version.diagnostic; return result; }
        bool supported = false;
        std::wistringstream versionLines(version.output); std::wstring versionLine;
        while (std::getline(versionLines, versionLine))
        {
            if (!versionLine.empty() && versionLine.back() == L'\r') versionLine.pop_back();
            if (versionLine == L"BleachBit version 6.0.6") supported = true;
        }
        if (!version.started || version.exitCode || version.timedOut || version.truncated || !version.diagnostic.empty() || !supported)
        { result.state = State::Unavailable; result.output = version.output; result.diagnostic = L"Only the audited complete BleachBit 6.0.6 Windows package is supported. " + version.diagnostic; return result; }
        result.version = L"6.0.6";
        const auto preview = Run(result.tool, std::wstring(Arguments) + L"--preview system.tmp", env, prepared->control.path, stop);
        result.output = preview.output;
        const auto summary = ReadSummary(preview, false); result.files = summary.files;
        if (preview.cancelled) { result.state = State::Cancelled; result.diagnostic = preview.diagnostic; return result; }
        if (!summary.valid || summary.errors || !ReadConfig(prepared->config.value, prepared->expectedConfig))
        { result.state = State::Failed; result.diagnostic = L"BleachBit preview was incomplete, reported errors, or the private configuration changed. Cleaning is disabled. " + preview.diagnostic; return result; }
        result.command = L"\"" + result.tool + L"\" " + Arguments + L"--clean system.tmp";
        result.diagnostic = L"External BleachBit system.tmp category restricted by its native keep list: only the current user's TEMP is cleaned, with all extensions and ages. Windows temporary files are protected, although they may be enumerated read-only. The engine enumerates again during cleaning; candidate checks do not select its files. Private configuration: " + prepared->configPath;
        result.state = State::Ready; result.prepared = std::move(prepared); return result;
    }

    Result Execute(const Preview& preview, const ExclusionPolicy& exclusions, const std::stop_token stop)
    {
        Result result;
        if (stop.stop_requested()) { result.state = State::Cancelled; result.diagnostic = L"Cancelled before cleaning started."; return result; }
        if (preview.state != State::Ready || !preview.prepared)
        { result.state = State::Blocked; result.diagnostic = L"A complete supported engine preview is required."; return result; }
        const auto& prepared = *preview.prepared;
        const auto command = L"\"" + prepared.tool.path + L"\" " + Arguments + L"--clean system.tmp";
        if (preview.version != L"6.0.6" || preview.tool != prepared.tool.path || preview.userTemp != prepared.user.path ||
            preview.systemTemp != prepared.system.path || preview.command != command ||
            !Unprivileged(result.diagnostic) || !exclusions.available || exclusions.text != prepared.exclusions.text ||
            Snapshot() != prepared.original || !CheckExclusions(exclusions, prepared.user.path, result.diagnostic) ||
            !SameLocation(prepared.tool, false, result.diagnostic) || !SameLocation(prepared.user, true, result.diagnostic) ||
            !SameLocation(prepared.system, true, result.diagnostic) || !SameLocation(prepared.control, true, result.diagnostic) ||
            !ReadConfig(prepared.config.value, prepared.expectedConfig))
        {
            result.state = State::Blocked;
            if (result.diagnostic.empty()) result.diagnostic = L"Exclusions, environment or private engine configuration changed after preview. Reopen the scope before confirming.";
            return result;
        }
        if (prepared.consumed.exchange(true))
        { result.state = State::Blocked; result.diagnostic = L"This preview has already been submitted. Create a new preview before another confirmed cleanup."; return result; }
        // The engine consumes its fixed category, never a serialized list of candidate paths.
        result.before = Measure(prepared.user.path);
        const auto cleaned = Run(prepared.tool.path, std::wstring(Arguments) + L"--clean system.tmp",
            prepared.environment, prepared.control.path, stop);
        result.started = cleaned.started; result.exitCode = cleaned.exitCode; result.output = cleaned.output;
        const auto summary = ReadSummary(cleaned, true); result.files = summary.files;
        if (summary.valid || summary.hasErrors) result.errors = summary.errors;
        result.after = Measure(prepared.user.path);
        if (cleaned.cancelled || stop.stop_requested())
        { result.state = State::Cancelled; result.diagnostic = L"Cleaning stopped. A started command may already have deleted some files; no rollback, automatic retry or fallback was performed. " + cleaned.diagnostic; return result; }
        const auto after = Run(prepared.tool.path, std::wstring(Arguments) + L"--preview system.tmp",
            prepared.environment, prepared.control.path, stop);
        result.postPreview = after.output;
        const auto remaining = ReadSummary(after, false); result.remainingFiles = remaining.files;
        if (after.cancelled || stop.stop_requested())
        { result.state = State::Cancelled; result.diagnostic = L"The cleanup command finished but its subsequent preview was cancelled; completed deletions cannot be undone."; return result; }
        const bool complete = summary.valid && !summary.errors && remaining.valid && !remaining.errors && remaining.files == 0 && ReadConfig(prepared.config.value, prepared.expectedConfig);
        result.state = complete ? State::Success : State::Failed;
        result.diagnostic = complete ?
            L"The engine reported no errors and its subsequent preview contains no deletable entries. Engine-reported recovered bytes are not an attributed volume-space measurement." :
            L"The command or subsequent preview failed, was incomplete, reported errors, or still lists entries. Some files may have been deleted; no retry or alternate deletion was performed. " + cleaned.diagnostic + L" " + after.diagnostic;
        return result;
    }
}
