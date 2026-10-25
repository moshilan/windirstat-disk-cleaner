// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include "NuGetHttpCache.h"
#include "CleanupPreview.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <utility>
#include <vector>

namespace Cleanup::NuGetHttp
{
    namespace
    {
        struct Handle
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            Handle() = default;
            explicit Handle(HANDLE h) : value(h) {}
            Handle(const Handle&) = delete;
            Handle& operator=(const Handle&) = delete;
            Handle(Handle&& h) noexcept : value(std::exchange(h.value, INVALID_HANDLE_VALUE)) {}
            ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
            explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
        };
        struct Identity
        {
            DWORD volume = 0, high = 0, low = 0;
            bool operator==(const Identity&) const = default;
        };
        bool Equal(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
        std::wstring Error(const wchar_t* action) { return std::wstring(action) + L" (Win32 " + std::to_wstring(GetLastError()) + L")"; }
        std::wstring Known(const KNOWNFOLDERID& id)
        {
            PWSTR path = nullptr;
            if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path))) return {};
            std::wstring result(path);
            CoTaskMemFree(path);
            return result;
        }
        struct EnvironmentLess
        {
            bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
        };
        using Environment = std::map<std::wstring, std::wstring, EnvironmentLess>;
        Environment Snapshot()
        {
            Environment result;
            const auto block = GetEnvironmentStringsW();
            if (!block) return result;
            for (const wchar_t* row = block; *row; row += wcslen(row) + 1)
            {
                const wchar_t* split = wcschr(row + 1, L'=');
                if (split) result.emplace(std::wstring(row, split), split + 1);
            }
            FreeEnvironmentStringsW(block);
            result[L"DOTNET_CLI_TELEMETRY_OPTOUT"] = L"1";
            result[L"DOTNET_SKIP_FIRST_TIME_EXPERIENCE"] = L"1";
            result[L"DOTNET_GENERATE_ASPNET_CERTIFICATE"] = L"false";
            result[L"DOTNET_ADD_GLOBAL_TOOLS_TO_PATH"] = L"false";
            result[L"DOTNET_DISABLE_GUI_ERRORS"] = L"1";
            result[L"DOTNET_NOLOGO"] = L"1";
            result[L"DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE"] = L"true";
            result[L"DOTNET_CLI_UI_LANGUAGE"] = L"en-US";
            result[L"DOTNET_CLI_FORCE_UTF8_ENCODING"] = L"1";
            for (const auto* name : { L"DOTNET_STARTUP_HOOKS", L"DOTNET_ADDITIONAL_DEPS", L"DOTNET_SHARED_STORE",
                L"CORECLR_ENABLE_PROFILING", L"COR_ENABLE_PROFILING", L"CORECLR_PROFILER_PATH", L"COR_PROFILER_PATH" })
                result.erase(name);
            return result;
        }
        std::vector<wchar_t> Block(const Environment& env)
        {
            std::vector<wchar_t> result;
            for (const auto& [name, value] : env)
            {
                const auto row = name + L"=" + value;
                result.insert(result.end(), row.begin(), row.end());
                result.push_back(0);
            }
            result.push_back(0);
            return result;
        }
        std::wstring Decode(const std::string& bytes)
        {
            if (bytes.empty()) return {};
            const int count = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            std::wstring result(count, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), result.data(), count);
            return result;
        }
        struct ProcessResult
        {
            bool started = false, cancelled = false, timedOut = false;
            bool outputTruncated = false;
            DWORD exitCode = ERROR_PROCESS_ABORTED;
            std::wstring output, error;
        };
        ProcessResult Run(const std::wstring& tool, const std::wstring& arguments, const Environment& env,
            const std::wstring& cwd, const std::stop_token stop)
        {
            ProcessResult result;
            if (stop.stop_requested()) { result.cancelled = true; return result; }
            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE read = nullptr, write = nullptr;
            if (!CreatePipe(&read, &write, &security, 0)) { result.error = Error(L"CreatePipe"); return result; }
            Handle reader(read), writer(write);
            SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
            Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
            Handle job(CreateJobObjectW(nullptr, nullptr));
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!input || !job || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            { result.error = Error(L"Create isolated command job"); return result; }
            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            std::vector<unsigned char> storage(bytes);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
            if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) { result.error = Error(L"Initialize process handles"); return result; }
            const std::array inherited{ write, input.value };
            if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr))
            { DeleteProcThreadAttributeList(attributes); result.error = Error(L"Restrict process handles"); return result; }
            STARTUPINFOEXW start{};
            start.StartupInfo.cb = sizeof(start);
            start.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            start.StartupInfo.hStdInput = input.value;
            start.StartupInfo.hStdOutput = start.StartupInfo.hStdError = write;
            start.lpAttributeList = attributes;
            auto environment = Block(env);
            auto command = L"\"" + tool + L"\" " + arguments;
            PROCESS_INFORMATION process{};
            const bool created = CreateProcessW(tool.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                environment.data(), cwd.c_str(), &start.StartupInfo, &process) != FALSE;
            DeleteProcThreadAttributeList(attributes);
            if (!created) { result.error = Error(L"Start official command"); return result; }
            Handle child(process.hProcess), thread(process.hThread);
            if (!AssignProcessToJobObject(job.value, process.hProcess))
            {
                result.error = Error(L"Bind official command to cancellation job");
                TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
                WaitForSingleObject(process.hProcess, 5000);
                return result;
            }
            if (stop.stop_requested() || ResumeThread(process.hThread) == static_cast<DWORD>(-1))
            {
                result.cancelled = stop.stop_requested();
                result.error = result.cancelled ? L"Cancelled before the official command started." : Error(L"Resume official command");
                TerminateJobObject(job.value, ERROR_CANCELLED);
                WaitForSingleObject(process.hProcess, 5000);
                return result;
            }
            result.started = true;
            CloseHandle(std::exchange(writer.value, INVALID_HANDLE_VALUE));
            std::string output;
            const auto started = GetTickCount64();
            auto drain = [&]
            {
                DWORD available = 0;
                std::array<char, 4096> buffer{};
                while (PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) && available)
                {
                    DWORD received = 0;
                    if (!ReadFile(reader.value, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &received, nullptr) || received == 0) break;
                    if (received > 65536 - output.size()) result.outputTruncated = true;
                    if (output.size() < 65536) output.append(buffer.data(), std::min<size_t>(received, 65536 - output.size()));
                }
            };
            while (WaitForSingleObject(child.value, 20) == WAIT_TIMEOUT)
            {
                drain();
                if (stop.stop_requested() || GetTickCount64() - started > 120000)
                {
                    result.cancelled = stop.stop_requested();
                    result.timedOut = !result.cancelled;
                    TerminateJobObject(job.value, ERROR_CANCELLED);
                    WaitForSingleObject(child.value, 5000);
                    break;
                }
            }
            drain();
            GetExitCodeProcess(child.value, &result.exitCode);
            result.output = Decode(output);
            if (result.cancelled || result.timedOut) result.error = result.cancelled ?
                L"Official command cancelled; some cache files may already have been removed." :
                L"Official command timed out; some cache files may already have been removed.";
            return result;
        }
        bool IdentityOf(const HANDLE file, Identity& id)
        {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(file, &info)) return false;
            id = { info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow };
            return true;
        }
        bool GuardAncestors(const std::wstring& path, std::vector<Handle>& guards)
        {
            auto current = std::filesystem::path(path).parent_path();
            std::vector<std::wstring> paths;
            while (!current.empty())
            {
                paths.push_back(current.wstring());
                const auto parent = current.parent_path();
                if (parent == current) break;
                current = parent;
            }
            std::ranges::reverse(paths);
            for (const auto& ancestor : paths)
            {
                Handle h(CreateFileW(ancestor.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                FILE_ATTRIBUTE_TAG_INFO info{};
                if (!h || !GetFileInformationByHandleEx(h.value, FileAttributeTagInfo, &info, sizeof(info)) ||
                    !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
                guards.push_back(std::move(h));
            }
            return true;
        }
        bool Inspect(const std::wstring& root, Measurement& measured, std::stop_token stop)
        {
            ULARGE_INTEGER available{};
            if (GetDiskFreeSpaceExW(std::filesystem::path(root).parent_path().c_str(), &available, nullptr, nullptr)) measured.availableBytes = available.QuadPart;
            const DWORD attributes = GetFileAttributesW(root.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const auto error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) { measured.logicalBytes = 0; return true; }
                return false;
            }
            constexpr DWORD unsafe = FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & unsafe)) return false;
            std::vector<std::wstring> directories{ root };
            std::uint64_t total = 0;
            size_t visited = 0;
            for (size_t i = 0; i < directories.size(); ++i)
            {
                WIN32_FIND_DATAW entry{};
                const auto find = FindFirstFileW((directories[i] + L"\\*").c_str(), &entry);
                if (find == INVALID_HANDLE_VALUE) return false;
                bool safe = true;
                do
                {
                    if (wcscmp(entry.cFileName, L".") == 0 || wcscmp(entry.cFileName, L"..") == 0) continue;
                    if (stop.stop_requested() || ++visited > 100000 || (entry.dwFileAttributes & unsafe)) { safe = false; break; }
                    const auto path = directories[i] + L"\\" + entry.cFileName;
                    if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) directories.push_back(path);
                    else
                    {
                        Handle file(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
                        BY_HANDLE_FILE_INFORMATION info{};
                        if (!file || !GetFileInformationByHandle(file.value, &info) || info.nNumberOfLinks != 1 ||
                            (info.dwFileAttributes & unsafe)) { safe = false; break; }
                        total += (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
                    }
                } while (FindNextFileW(find, &entry));
                const DWORD error = GetLastError();
                FindClose(find);
                if (!safe || error != ERROR_NO_MORE_FILES) return false;
            }
            measured.logicalBytes = total;
            return true;
        }
        std::wstring ListedPath(const std::wstring& output, const std::wstring& name = L"http-cache")
        {
            const auto prefix = name + L": ";
            const auto begin = output.find(prefix);
            if (begin == std::wstring::npos || (begin && output[begin - 1] != L'\n')) return {};
            const auto start = begin + prefix.size();
            const auto end = output.find_first_of(L"\r\n", start);
            std::wstring normalized, error;
            if (!NormalizeDirectory(output.substr(start, end - start), normalized, error)) return {};
            return normalized;
        }
        bool SeparateCaches(const std::wstring& output, const std::wstring& http)
        {
            for (const auto* name : { L"global-packages", L"temp", L"plugins-cache" })
            {
                const auto other = ListedPath(output, name);
                if (other.empty() || IsWithin(http, other) || IsWithin(other, http)) return false;
                // Lexically separate caches may alias the HTTP tree through a junction.
                // Accept missing tails, but refuse existing linked/unknown ancestors.
                for (size_t end = 3; ;)
                {
                    const auto prefix = other.substr(0, end);
                    const auto attributes = GetFileAttributesW(prefix.c_str());
                    if (attributes == INVALID_FILE_ATTRIBUTES)
                    {
                        const auto error = GetLastError();
                        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) break;
                        return false;
                    }
                    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                        FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) return false;
                    if (end == other.size()) break;
                    end = other.find(L'\\', end + 1);
                    if (end == std::wstring::npos) end = other.size();
                }
            }
            return true;
        }
        std::wstring PhysicalDirectory(const std::wstring& path)
        {
            Handle directory(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            FILE_ATTRIBUTE_TAG_INFO info{};
            if (!directory || !GetFileInformationByHandleEx(directory.value, FileAttributeTagInfo, &info, sizeof(info)) ||
                !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
            std::wstring value(32768, L'\0');
            const auto length = GetFinalPathNameByHandleW(directory.value, value.data(), static_cast<DWORD>(value.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (length == 0 || length >= value.size()) return {};
            value.resize(length);
            if (!value.starts_with(L"\\\\?\\") || value.starts_with(L"\\\\?\\UNC\\")) return {};
            value.erase(0, 4);
            if (value.size() == 3 && value[1] == L':') return value;
            std::wstring normalized, error;
            return NormalizeDirectory(value, normalized, error) ? normalized : L"";
        }
        ExclusionFailure CheckExclusions(const std::wstring& root, const ExclusionPolicy& policy,
            std::wstring& excluded, std::wstring& diagnostic)
        {
            std::vector<std::wstring> directories;
            std::wstring error;
            if (!policy.available || !ParseExclusions(policy.text, directories, error))
            { diagnostic = L"Exclusion rules are unavailable or invalid. No clear command was started. " + error; return ExclusionFailure::Unavailable; }
            const auto physicalRoot = root.empty() ? L"" : PhysicalDirectory(root);
            if (!root.empty() && physicalRoot.empty())
            { diagnostic = L"The actual HTTP cache path cannot be resolved for exclusion checks."; return ExclusionFailure::Unavailable; }
            for (const auto& directory : directories)
            {
                // Resolve existing 8.3 ancestors even when the excluded child does not exist.
                auto existing = std::filesystem::path(directory);
                DWORD attributes = INVALID_FILE_ATTRIBUTES;
                while ((attributes = GetFileAttributesW(existing.c_str())) == INVALID_FILE_ATTRIBUTES)
                {
                    const auto code = GetLastError();
                    const auto parent = existing.parent_path();
                    if ((code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) || parent == existing || parent.empty()) break;
                    existing = parent;
                }
                std::wstring canonical;
                if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
                        FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) ||
                    (existing != existing.root_path() && !HasSafeAncestors(existing.wstring())) ||
                    (canonical = PhysicalDirectory(existing.wstring())).empty())
                { excluded = directory; diagnostic = L"An excluded directory cannot be reliably checked: " + directory; return ExclusionFailure::Unavailable; }
                // Joining through filesystem::path preserves the separator for a drive-root ancestor.
                const auto tail = std::filesystem::path(directory).lexically_relative(existing);
                const auto resolved = (std::filesystem::path(canonical) / tail).lexically_normal().wstring();
                if (!root.empty() && (IsWithin(physicalRoot, resolved) || IsWithin(resolved, physicalRoot)))
                { excluded = directory; diagnostic = L"The entire HTTP cache overlaps an excluded directory: " + directory; return ExclusionFailure::Overlap; }
            }
            return ExclusionFailure::None;
        }
        // An explicit empty --python overrides configuration-driven interpreter redirection (pip 22.3+).
        constexpr auto PipArguments = L"-I -B -X utf8 -m pip --python= --disable-pip-version-check --no-input --log NUL ";
        Environment PipEnvironment()
        {
            auto env = Snapshot();
            env[L"PIP_DISABLE_PIP_VERSION_CHECK"] = L"1";
            env[L"PIP_NO_INPUT"] = L"1";
            env[L"PYTHONDONTWRITEBYTECODE"] = L"1";
            if (const auto config = env.find(L"PIP_CONFIG_FILE"); config != env.end() && Equal(config->second, L"NUL"))
                config->second = L"nul"; // pip compares this spelling to Python's os.devnull literally.
            return env;
        }
        std::wstring PipEnvironmentFingerprint(const Environment& env)
        {
            std::wstring result;
            for (const auto& [key, value] : env)
                if (_wcsnicmp(key.c_str(), L"PIP_", 4) == 0 || _wcsnicmp(key.c_str(), L"PYTHON", 6) == 0 ||
                    Equal(key, L"VIRTUAL_ENV") || Equal(key, L"CONDA_PREFIX")) result += key + L"=" + value + L"\n";
            return result;
        }
        struct ToolQuery
        {
            bool valid = false, cancelled = false;
            std::wstring root, details, fingerprint;
        };
        ToolQuery QueryPip(const std::wstring& tool, const Environment& env, const std::wstring& cwd, const std::stop_token stop)
        {
            ToolQuery result;
            // The chosen interpreter is explicit. Isolated mode excludes CWD, PYTHONPATH and user-site packages.
            const auto identity = Run(tool, L"-I -B -X utf8 -c \"import sys,pip;print('executable: '+sys.executable);"
                L"print('prefix: '+sys.prefix);print('base-prefix: '+sys.base_prefix);"
                L"print('Python: '+sys.version.split()[0]);print('pip: '+pip.__version__);print('pip-module: '+pip.__file__)\"", env, cwd, stop);
            result.cancelled = identity.cancelled;
            if (!identity.started || identity.exitCode != 0 || identity.outputTruncated || identity.cancelled || identity.timedOut ||
                !Equal(ListedPath(identity.output, L"executable"), tool) || ListedPath(identity.output, L"pip-module").empty()) return result;
            const auto config = Run(tool, std::wstring(PipArguments) + L"config list", env, cwd, stop);
            result.cancelled = config.cancelled;
            // Configuration may contain credentials. Keep it only in memory; never return it as diagnostics/output.
            if (!config.started || config.exitCode != 0 || config.outputTruncated || config.cancelled || config.timedOut) return result;
            const auto location = Run(tool, std::wstring(PipArguments) + L"cache dir", env, cwd, stop);
            result.cancelled = location.cancelled;
            std::wstring text = location.output, error;
            while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
            if (!location.started || location.exitCode != 0 || location.outputTruncated || location.cancelled || location.timedOut ||
                !NormalizeDirectory(text, result.root, error)) return result;
            result.details = identity.output;
            result.fingerprint = identity.output + L"\n" + config.output;
            result.valid = true;
            return result;
        }
        constexpr auto NpmArguments = L"--global --logs-max=0 --logs-dir=\\\\.\\NUL --timing=false --loglevel=error "
            L"--no-update-notifier --ignore-scripts --no-audit --no-fund --color=false --progress=false --json=false ";
        Environment NpmEnvironment()
        {
            auto env = Snapshot();
            for (const auto* name : { L"NODE_OPTIONS", L"NODE_PATH", L"NODE_V8_COVERAGE", L"NODE_COMPILE_CACHE", L"NODE_DEBUG", L"NODE_DEBUG_NATIVE" }) env.erase(name);
            env[L"NODE_DISABLE_COMPILE_CACHE"] = L"1";
            return env;
        }
        std::wstring NpmEnvironmentFingerprint()
        {
            std::wstring result;
            for (const auto& [key, value] : Snapshot())
                if (_wcsnicmp(key.c_str(), L"NPM_", 4) == 0 || _wcsnicmp(key.c_str(), L"NODE_", 5) == 0 ||
                    Equal(key, L"HOME") || Equal(key, L"USERPROFILE") || Equal(key, L"APPDATA") || Equal(key, L"LOCALAPPDATA") ||
                    Equal(key, L"HOMEDRIVE") || Equal(key, L"HOMEPATH")) result += key + L"=" + value + L"\n";
            return result;
        }
        ToolQuery QueryNpm(const std::wstring& tool, const std::wstring& cli, const Environment& env,
            const std::wstring& cwd, const std::stop_token stop)
        {
            ToolQuery result;
            // npm CLI queries create caches and rotate old logs, even with logs-max=0. Load only its
            // configuration parser here; no Npm instance, cache mkdir, log manager, verify or GC runs.
            const auto probe = Run(tool, std::wstring(L"--no-warnings -e \"") +
                L"const p=require('node:path'),fs=require('node:fs/promises'),h=require('node:crypto');"
                L"(async()=>{const cli=process.argv[1],r=p.dirname(p.dirname(cli)),v=require(p.join(r,'package.json')).version;"
                L"if(!/^(10|11)\\./.test(v))throw Error();const cp=p.join(r,'node_modules','@npmcli','config'),C=require(cp),D=require(p.join(cp,'lib','definitions'));"
                L"const device=String.fromCharCode(92,92,46,92)+'NUL';"
                L"const a=['--global','--logs-max=0','--logs-dir='+device,'--timing=false','--loglevel=error','--no-update-notifier','--ignore-scripts','--no-audit','--no-fund','--color=false','--progress=false','--json=false'];"
                L"const c=new C({...D,npmPath:r,argv:[process.execPath,cli,...a],warn:false});await c.load();"
                L"if(!c.valid||[...c.data.values()].some(d=>d.loadError&&d.loadError.code!=='ENOENT'))throw Error();"
                L"if(p.resolve(c.flat.cache).toLowerCase()!==p.resolve(c.get('cache'),'_cacache').toLowerCase())throw Error();"
                L"if(c.get('logs-max')!==0||c.get('timing')!==false||p.resolve(c.get('logs-dir')).toLowerCase()!==p.resolve(device).toLowerCase())throw Error();"
                L"let bad=false;try{await fs.readdir(c.get('logs-dir'));bad=true}catch(e){}if(bad)throw Error();"
                L"console.log('executable: '+process.execPath);console.log('node: '+process.version);console.log('npm: '+v);console.log('npm-cli: '+cli);console.log('cache: '+c.get('cache'));"
                L"console.log('fingerprint: '+h.createHash('sha256').update(JSON.stringify([...c.data].map(([k,d])=>[k,d.source,d.raw,Object.entries(d.data)]))).digest('hex'))"
                L"})().catch(()=>{process.exitCode=1})\" \"" + cli + L"\"", env, cwd, stop);
            result.cancelled = probe.cancelled;
            if (!probe.started || probe.exitCode != 0 || probe.cancelled || probe.timedOut || probe.outputTruncated ||
                !Equal(ListedPath(probe.output, L"executable"), tool) || !Equal(ListedPath(probe.output, L"npm-cli"), cli)) return result;
            result.root = ListedPath(probe.output, L"cache");
            const auto fingerprint = probe.output.find(L"fingerprint: ");
            if (result.root.empty() || fingerprint == std::wstring::npos) return result;
            result.details = probe.output.substr(0, fingerprint);
            result.fingerprint = probe.output; // Only safe identity fields and a digest leave the child process.
            result.valid = true;
            return result;
        }
    }

    struct Prepared
    {
        Provider provider = Provider::NuGetHttp;
        std::wstring tool, root, command, cwd;
        std::wstring environmentDetails, pipFingerprint, pipEnvironmentFingerprint;
        std::wstring npmCli, npmFingerprint, npmEnvironmentFingerprint;
        Environment environment;
        Identity toolIdentity, rootIdentity, cliIdentity;
        Handle toolGuard, cliGuard;
        ExclusionPolicy exclusions;
    };

    Preview Prepare(const ExclusionPolicy& exclusions, const std::stop_token stop)
    {
        Preview result;
        auto prepared = std::make_shared<Prepared>();
        prepared->environment = Snapshot();
        if (stop.stop_requested()) { result.state = State::Cancelled; return result; }
        prepared->exclusions = exclusions;
        result.exclusionFailure = CheckExclusions(L"", exclusions, result.excludedDirectory, result.diagnostic);
        if (result.exclusionFailure != ExclusionFailure::None) { result.state = State::Blocked; return result; }
        const auto programFiles = Known(FOLDERID_ProgramFiles);
        prepared->tool = programFiles.empty() ? L"" : programFiles + L"\\dotnet\\dotnet.exe";
        result.tool = prepared->tool;
        prepared->toolGuard.value = CreateFileW(prepared->tool.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (!prepared->toolGuard || !IdentityOf(prepared->toolGuard.value, prepared->toolIdentity) || !HasSafeAncestors(prepared->tool))
        {
            result.diagnostic = L"A usable system-installed .NET SDK command was not found. No dependencies will be installed. PATH-only nuget.exe is not trusted by this pilot.";
            return result;
        }
        prepared->cwd = std::filesystem::path(prepared->tool).root_path().wstring();
        const auto fixture = prepared->environment.find(L"WDS_CLEANUP_TEST_ROOT");
        std::wstring expected;
        if (fixture != prepared->environment.end())
        {
            std::wstring local, error;
            if (!ResolveTestFixture(fixture->second, local, error))
            { result.state = State::Blocked; result.diagnostic = L"Invalid isolated fixture: " + error; return result; }
            expected = fixture->second + L"\\NuGetHttp";
            const auto cache = prepared->environment.find(L"NUGET_HTTP_CACHE_PATH");
            if (cache == prepared->environment.end() || !Equal(cache->second, expected))
            { result.state = State::Blocked; result.diagnostic = L"The isolated fixture requires its own explicit NuGetHttp cache; real user cache fallback is forbidden."; return result; }
            prepared->environment[L"DOTNET_CLI_HOME"] = fixture->second + L"\\CliHome";
            prepared->environment[L"NUGET_PACKAGES"] = fixture->second + L"\\Packages";
            prepared->environment[L"NUGET_SCRATCH"] = fixture->second + L"\\Scratch";
            prepared->environment[L"NUGET_PLUGINS_CACHE_PATH"] = fixture->second + L"\\Plugins";
        }
        else
        {
            const auto local = Known(FOLDERID_LocalAppData);
            if (!local.empty()) expected = local + L"\\NuGet\\v3-cache";
        }
        // Runner TEMP can use an 8.3 alias; compare both sides in the same canonical form.
        std::wstring normalizedExpected, pathError;
        if (!NormalizeDirectory(expected, normalizedExpected, pathError))
        { result.state = State::Blocked; result.diagnostic = L"Expected HTTP cache path is invalid: " + pathError; return result; }
        expected = std::move(normalizedExpected);
        // Listing all locations is read-only and detects dangerous cache-location overlaps.
        const auto listed = Run(prepared->tool, L"nuget locals all --list --force-english-output", prepared->environment, prepared->cwd, stop);
        if (listed.cancelled) { result.state = State::Cancelled; return result; }
        if (!listed.started || listed.exitCode != 0 || (prepared->root = ListedPath(listed.output)).empty())
        { result.diagnostic = L"The installed .NET command cannot list the NuGet HTTP cache. An SDK may be missing. " + listed.error + L"\r\n" + listed.output; return result; }
        result.cachePath = prepared->root;
        prepared->command = L"\"" + prepared->tool + L"\" nuget locals http-cache --clear --force-english-output";
        result.command = prepared->command;
        result.exclusionFailure = CheckExclusions(prepared->root, exclusions, result.excludedDirectory, result.diagnostic);
        if (result.exclusionFailure != ExclusionFailure::None) { result.state = State::Blocked; return result; }
        if (!SeparateCaches(listed.output, prepared->root))
        { result.state = State::Blocked; result.diagnostic = L"NuGet cache locations overlap or could not all be resolved. HTTP-only cleanup is blocked to preserve other caches."; return result; }
        if (expected.empty() || !Equal(expected, prepared->root))
        { result.state = State::Blocked; result.diagnostic = L"This pilot only clears the standard HTTP cache location. A custom or unexpected actual location is blocked.\r\nExpected: " + expected + L"\r\nActual: " + prepared->root; return result; }
        prepared->environment[L"NUGET_HTTP_CACHE_PATH"] = prepared->root;
        std::vector<Handle> ancestors;
        if (!GuardAncestors(prepared->root, ancestors) || !HasSafeAncestors(prepared->root) || !Inspect(prepared->root, result.before, stop))
        { result.state = stop.stop_requested() ? State::Cancelled : State::Blocked; result.diagnostic = L"Cache path, ancestor, reparse point, hard link, access or inspection limit check failed. No clear command was started."; return result; }
        Handle root(CreateFileW(prepared->root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!root || !IdentityOf(root.value, prepared->rootIdentity))
        { result.state = State::Blocked; result.diagnostic = L"The HTTP cache directory is absent or unavailable; nothing will be created or cleared."; return result; }
        result.state = State::Ready;
        result.prepared = std::move(prepared);
        return result;
    }

    Preview PreparePip(const ExclusionPolicy& exclusions, const std::wstring& absolutePython, const std::stop_token stop)
    {
        Preview result;
        result.provider = Provider::Pip;
        auto prepared = std::make_shared<Prepared>();
        prepared->provider = Provider::Pip;
        prepared->environment = PipEnvironment();
        prepared->pipEnvironmentFingerprint = PipEnvironmentFingerprint(prepared->environment);
        prepared->exclusions = exclusions;
        if (stop.stop_requested()) { result.state = State::Cancelled; return result; }
        result.exclusionFailure = CheckExclusions(L"", exclusions, result.excludedDirectory, result.diagnostic);
        if (result.exclusionFailure != ExclusionFailure::None) { result.state = State::Blocked; return result; }
        std::wstring error;
        if (!NormalizeDirectory(absolutePython, prepared->tool, error) ||
            !Equal(std::filesystem::path(prepared->tool).filename().wstring(), L"python.exe") || !HasSafeAncestors(prepared->tool))
        { result.state = State::Blocked; result.diagnostic = L"Select an existing absolute python.exe from the intended trusted environment. No PATH or launcher fallback is used."; return result; }
        result.tool = prepared->tool;
        prepared->toolGuard.value = CreateFileW(prepared->tool.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (!prepared->toolGuard || !IdentityOf(prepared->toolGuard.value, prepared->toolIdentity))
        { result.diagnostic = L"The selected Python interpreter is unavailable. No dependencies will be installed."; return result; }
        prepared->cwd = std::filesystem::path(prepared->tool).root_path().wstring();
        std::wstring expected;
        const auto fixture = prepared->environment.find(L"WDS_CLEANUP_TEST_ROOT");
        if (fixture != prepared->environment.end())
        {
            std::wstring local;
            if (!ResolveTestFixture(fixture->second, local, error))
            { result.state = State::Blocked; result.diagnostic = L"Invalid isolated fixture: " + error; return result; }
            expected = fixture->second + L"\\PipCache";
            const auto cache = prepared->environment.find(L"PIP_CACHE_DIR");
            const auto config = prepared->environment.find(L"PIP_CONFIG_FILE");
            std::wstring actualCache, configPath, fixturePath, fixtureCache;
            if (cache == prepared->environment.end() || !NormalizeDirectory(cache->second, actualCache, error) ||
                !NormalizeDirectory(expected, fixtureCache, error) || !Equal(actualCache, fixtureCache) || config == prepared->environment.end() ||
                (!Equal(config->second, L"NUL") && (!NormalizeDirectory(config->second, configPath, error) ||
                    !NormalizeDirectory(fixture->second, fixturePath, error) || !IsWithin(configPath, fixturePath) || !HasSafeAncestors(configPath))))
            { result.state = State::Blocked; result.diagnostic = L"The isolated fixture requires its own explicit PipCache and NUL or fixture-owned pip configuration. Real-cache fallback is forbidden."; return result; }
            expected = std::move(fixtureCache);
        }
        else
        {
            const auto local = Known(FOLDERID_LocalAppData);
            if (!local.empty()) expected = local + L"\\pip\\Cache";
        }
        std::wstring normalizedExpected;
        if (!NormalizeDirectory(expected, normalizedExpected, error))
        { result.state = State::Blocked; result.diagnostic = L"The expected standard pip cache is unavailable."; return result; }
        const auto query = QueryPip(prepared->tool, prepared->environment, prepared->cwd, stop);
        if (query.cancelled) { result.state = State::Cancelled; return result; }
        if (!query.valid)
        { result.diagnostic = L"The selected interpreter cannot reliably query pip and its cache. pip 22.3+ in this isolated Python environment is required; nothing will be installed or switched automatically."; return result; }
        prepared->root = query.root;
        prepared->environmentDetails = query.details;
        prepared->pipFingerprint = query.fingerprint;
        prepared->command = L"\"" + prepared->tool + L"\" " + PipArguments + L"--cache-dir \"" + prepared->root + L"\" cache purge";
        result.cachePath = prepared->root;
        result.command = prepared->command;
        result.environmentDetails = prepared->environmentDetails;
        if (!Equal(normalizedExpected, prepared->root))
        { result.state = State::Blocked; result.diagnostic = L"This pilot only clears the standard pip cache (or validated fixture). A custom or unexpected actual cache directory is blocked."; return result; }
        if (IsWithin(prepared->tool, prepared->root) || IsWithin(ListedPath(query.details, L"pip-module"), prepared->root) ||
            IsWithin(ListedPath(query.details, L"prefix"), prepared->root))
        { result.state = State::Blocked; result.diagnostic = L"The pip cache overlaps the selected Python environment; cleanup is blocked."; return result; }
        result.exclusionFailure = CheckExclusions(prepared->root, exclusions, result.excludedDirectory, result.diagnostic);
        if (result.exclusionFailure != ExclusionFailure::None) { result.state = State::Blocked; return result; }
        std::vector<Handle> ancestors;
        if (!GuardAncestors(prepared->root, ancestors) || !HasSafeAncestors(prepared->root) || !Inspect(prepared->root, result.before, stop))
        { result.state = stop.stop_requested() ? State::Cancelled : State::Blocked; result.diagnostic = L"Pip cache path, reparse point, hard link, access or inspection limit check failed. No purge command started."; return result; }
        Handle root(CreateFileW(prepared->root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!root || !IdentityOf(root.value, prepared->rootIdentity))
        { result.state = State::Blocked; result.diagnostic = L"The pip cache directory is absent or unavailable. No directory will be created."; return result; }
        result.state = State::Ready;
        result.prepared = std::move(prepared);
        return result;
    }

    Preview PrepareNpm(const ExclusionPolicy& exclusions, const std::wstring& absoluteNode, const std::stop_token stop)
    {
        Preview result;
        result.provider = Provider::Npm;
        auto prepared = std::make_shared<Prepared>();
        prepared->provider = Provider::Npm;
        prepared->environment = NpmEnvironment();
        prepared->npmEnvironmentFingerprint = NpmEnvironmentFingerprint();
        prepared->exclusions = exclusions;
        if (stop.stop_requested()) { result.state = State::Cancelled; return result; }
        result.exclusionFailure = CheckExclusions(L"", exclusions, result.excludedDirectory, result.diagnostic);
        if (result.exclusionFailure != ExclusionFailure::None) { result.state = State::Blocked; return result; }
        std::wstring error;
        if (!NormalizeDirectory(absoluteNode, prepared->tool, error) ||
            !Equal(std::filesystem::path(prepared->tool).filename().wstring(), L"node.exe") || !HasSafeAncestors(prepared->tool))
        { result.state = State::Blocked; result.diagnostic = L"Select the intended existing absolute node.exe. No PATH, npm.cmd or launcher fallback is used."; return result; }
        result.tool = prepared->tool;
        const auto cli = (std::filesystem::path(prepared->tool).parent_path() / L"node_modules/npm/bin/npm-cli.js").wstring();
        if (!NormalizeDirectory(cli, prepared->npmCli, error) || !HasSafeAncestors(prepared->npmCli))
        { result.diagnostic = L"npm-cli.js is unavailable beside the selected Node installation. Nothing will be installed or selected automatically."; return result; }
        prepared->toolGuard.value = CreateFileW(prepared->tool.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        prepared->cliGuard.value = CreateFileW(prepared->npmCli.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        std::vector<Handle> installation;
        if (!prepared->toolGuard || !prepared->cliGuard || !IdentityOf(prepared->toolGuard.value, prepared->toolIdentity) ||
            !IdentityOf(prepared->cliGuard.value, prepared->cliIdentity) || !GuardAncestors(prepared->tool, installation) || !GuardAncestors(prepared->npmCli, installation))
        { result.state = State::Blocked; result.diagnostic = L"The selected Node/npm installation cannot be safely identified or pinned."; return result; }
        prepared->cwd = std::filesystem::path(prepared->tool).root_path().wstring();
        std::wstring expected;
        if (const auto fixture = prepared->environment.find(L"WDS_CLEANUP_TEST_ROOT"); fixture != prepared->environment.end())
        {
            std::wstring local, fixturePath;
            if (!ResolveTestFixture(fixture->second, local, error) || !NormalizeDirectory(fixture->second, fixturePath, error))
            { result.state = State::Blocked; result.diagnostic = L"The marked synthetic fixture is invalid. Real-cache fallback is forbidden."; return result; }
            expected = fixturePath + L"\\NpmCache";
            const auto cache = prepared->environment.find(L"NPM_CONFIG_CACHE");
            std::wstring actual;
            if (cache == prepared->environment.end() || !NormalizeDirectory(cache->second, actual, error) || !Equal(actual, expected))
            { result.state = State::Blocked; result.diagnostic = L"The synthetic npm operation requires its explicit fixture-owned NpmCache."; return result; }
            for (const auto* key : { L"NPM_CONFIG_USERCONFIG", L"NPM_CONFIG_GLOBALCONFIG" })
            {
                const auto file = prepared->environment.find(key);
                std::wstring path;
                if (file == prepared->environment.end() || !NormalizeDirectory(file->second, path, error) ||
                    !IsWithin(path, fixturePath) || !HasSafeAncestors(path))
                { result.state = State::Blocked; result.diagnostic = L"Synthetic npm configuration must be explicit safe files owned by the fixture."; return result; }
            }
        }
        else
        {
            const auto local = Known(FOLDERID_LocalAppData);
            if (!local.empty()) expected = local + L"\\npm-cache";
        }
        std::wstring normalizedExpected;
        if (!NormalizeDirectory(expected, normalizedExpected, error))
        { result.state = State::Blocked; result.diagnostic = L"The standard npm cache location cannot be established."; return result; }
        const auto query = QueryNpm(prepared->tool, prepared->npmCli, prepared->environment, prepared->cwd, stop);
        if (query.cancelled) { result.state = State::Cancelled; return result; }
        if (!query.valid)
        { result.diagnostic = L"The selected npm configuration cannot be read safely. This pilot supports npm 10/11 with the expected configuration parser and _cacache layout; no CLI verification or repair was run."; return result; }
        prepared->root = query.root;
        prepared->environmentDetails = query.details;
        prepared->npmFingerprint = query.fingerprint;
        prepared->command = L"\"" + prepared->tool + L"\" \"" + prepared->npmCli + L"\" " + NpmArguments + L"--cache \"" + prepared->root + L"\" cache clean --force";
        result.cachePath = prepared->root;
        result.command = prepared->command;
        result.environmentDetails = prepared->environmentDetails;
        if (!Equal(prepared->root, normalizedExpected))
        { result.state = State::Blocked; result.diagnostic = L"Only the actual standard npm cache or a validated fixture is supported; a custom location is blocked."; return result; }
        if (IsWithin(prepared->tool, prepared->root) || IsWithin(prepared->npmCli, prepared->root))
        { result.state = State::Blocked; result.diagnostic = L"The npm cache overlaps its selected installation."; return result; }
        result.exclusionFailure = CheckExclusions(prepared->root, exclusions, result.excludedDirectory, result.diagnostic);
        if (result.exclusionFailure != ExclusionFailure::None) { result.state = State::Blocked; return result; }
        std::vector<Handle> ancestors;
        if (!GuardAncestors(prepared->root, ancestors) || !HasSafeAncestors(prepared->root) || !Inspect(prepared->root, result.before, stop))
        { result.state = stop.stop_requested() ? State::Cancelled : State::Blocked; result.diagnostic = L"The npm cache contains an unsafe path, reparse point, hard link, access failure or exceeds the inspection limit."; return result; }
        Handle root(CreateFileW(prepared->root.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!root || !IdentityOf(root.value, prepared->rootIdentity))
        { result.state = State::Blocked; result.diagnostic = L"The npm cache is missing or unavailable; no directory is created during detection."; return result; }
        result.state = State::Ready;
        result.prepared = std::move(prepared);
        return result;
    }

    Result Execute(const Preview& preview, const ExclusionPolicy& currentExclusions, const std::stop_token stop)
    {
        Result result;
        result.before = preview.before;
        if (stop.stop_requested()) { result.state = State::Cancelled; result.diagnostic = L"Cancelled before starting the clear command."; return result; }
        const auto prepared = preview.prepared;
        if (preview.state != State::Ready || !prepared || preview.provider != prepared->provider || preview.tool != prepared->tool ||
            preview.cachePath != prepared->root || preview.command != prepared->command || preview.environmentDetails != prepared->environmentDetails)
        { result.state = State::Blocked; result.diagnostic = L"The displayed official-command scope is invalid or changed."; return result; }
        result.exclusionFailure = CheckExclusions(prepared->root, currentExclusions, result.excludedDirectory, result.diagnostic);
        if (result.exclusionFailure == ExclusionFailure::None && currentExclusions.text != prepared->exclusions.text)
        { result.exclusionFailure = ExclusionFailure::Changed; result.diagnostic = L"Exclusion rules changed after preview. Reopen the official scope before confirming again."; }
        if (result.exclusionFailure != ExclusionFailure::None) { result.state = State::Blocked; return result; }
        std::vector<Handle> ancestors;
        Identity toolIdentity, rootIdentity;
        // pip/npm purge contents without deleting the configured root. Keep its name stable.
        const DWORD rootShare = FILE_SHARE_READ | FILE_SHARE_WRITE | (prepared->provider == Provider::NuGetHttp ? FILE_SHARE_DELETE : 0);
        Handle root(CreateFileW(prepared->root.c_str(), FILE_READ_ATTRIBUTES, rootShare,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!IdentityOf(prepared->toolGuard.value, toolIdentity) || toolIdentity != prepared->toolIdentity || !root ||
            !IdentityOf(root.value, rootIdentity) || rootIdentity != prepared->rootIdentity ||
            !GuardAncestors(prepared->root, ancestors) || !HasSafeAncestors(prepared->root))
        { result.state = stop.stop_requested() ? State::Cancelled : State::Blocked; result.diagnostic = L"The cache identity or safety conditions changed after preview. The clear command was not started."; return result; }
        if (prepared->provider == Provider::Pip)
        {
            if (PipEnvironmentFingerprint(PipEnvironment()) != prepared->pipEnvironmentFingerprint)
            { result.state = State::Blocked; result.diagnostic = L"The pip/Python environment changed after preview. No purge command started."; return result; }
        }
        else if (prepared->provider == Provider::NuGetHttp)
        {
            const auto listed = Run(prepared->tool, L"nuget locals all --list --force-english-output", prepared->environment, prepared->cwd, stop);
            if (listed.cancelled) { result.state = State::Cancelled; return result; }
            if (listed.exitCode != 0 || !Equal(ListedPath(listed.output), prepared->root) || !SeparateCaches(listed.output, prepared->root))
            { result.state = State::Blocked; result.diagnostic = L"The official HTTP cache scope could not be reconfirmed."; return result; }
        }
        if (prepared->provider == Provider::Npm)
        {
            Identity currentTool, currentCli, heldCli;
            if (!GuardAncestors(prepared->tool, ancestors) || !GuardAncestors(prepared->npmCli, ancestors) ||
                !HasSafeAncestors(prepared->tool) || !HasSafeAncestors(prepared->npmCli))
            { result.state = State::Blocked; result.diagnostic = L"The selected Node/npm installation path became unsafe."; return result; }
            Handle tool(CreateFileW(prepared->tool.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
            Handle cli(CreateFileW(prepared->npmCli.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!tool || !cli || !IdentityOf(tool.value, currentTool) || currentTool != prepared->toolIdentity ||
                !IdentityOf(cli.value, currentCli) || currentCli != prepared->cliIdentity ||
                !IdentityOf(prepared->cliGuard.value, heldCli) || heldCli != prepared->cliIdentity)
            { result.state = State::Blocked; result.diagnostic = L"Node or npm-cli.js identity changed after scope review."; return result; }
        }
        Handle finalRoot(CreateFileW(prepared->root.c_str(), FILE_READ_ATTRIBUTES, rootShare,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!finalRoot || !IdentityOf(finalRoot.value, rootIdentity) || rootIdentity != prepared->rootIdentity ||
            !HasSafeAncestors(prepared->root) || !Inspect(prepared->root, result.before, stop))
        { result.state = stop.stop_requested() ? State::Cancelled : State::Blocked; result.diagnostic = L"The cache changed or became unsafe while reconfirming the official scope. No clear command was started."; return result; }
        if (prepared->provider == Provider::NuGetHttp)
        {
            CloseHandle(std::exchange(finalRoot.value, INVALID_HANDLE_VALUE));
            CloseHandle(std::exchange(root.value, INVALID_HANDLE_VALUE));
        }
        if (prepared->provider == Provider::Pip)
        {
            const auto query = QueryPip(prepared->tool, prepared->environment, prepared->cwd, stop);
            if (query.cancelled) { result.state = State::Cancelled; return result; }
            if (!query.valid || !Equal(query.root, prepared->root) || query.fingerprint != prepared->pipFingerprint ||
                PipEnvironmentFingerprint(PipEnvironment()) != prepared->pipEnvironmentFingerprint)
            { result.state = State::Blocked; result.diagnostic = L"The selected Python, pip configuration or cache scope changed or cannot be reconfirmed. No purge command started."; return result; }
        }
        if (prepared->provider == Provider::Npm)
        {
            const auto query = QueryNpm(prepared->tool, prepared->npmCli, prepared->environment, prepared->cwd, stop);
            if (query.cancelled) { result.state = State::Cancelled; return result; }
            if (!query.valid || !Equal(query.root, prepared->root) || query.fingerprint != prepared->npmFingerprint ||
                NpmEnvironmentFingerprint() != prepared->npmEnvironmentFingerprint)
            { result.state = State::Blocked; result.diagnostic = L"The npm/Node environment, configuration or actual cache changed or cannot be reconfirmed. No clean command started."; return result; }
        }
        result.exclusionFailure = CheckExclusions(prepared->root, currentExclusions, result.excludedDirectory, result.diagnostic);
        if (result.exclusionFailure != ExclusionFailure::None) { result.state = State::Blocked; return result; }
        // Official path-based deletion is not atomic. Cancellation may leave partial completion.
        const auto arguments = prepared->provider == Provider::Pip ? std::wstring(PipArguments) + L"--cache-dir \"" + prepared->root + L"\" cache purge" :
            prepared->provider == Provider::Npm ? L"\"" + prepared->npmCli + L"\" " + NpmArguments + L"--cache \"" + prepared->root + L"\" cache clean --force" : L"nuget locals http-cache --clear --force-english-output";
        const auto run = Run(prepared->tool, arguments, prepared->environment, prepared->cwd, stop);
        result.started = run.started;
        result.exitCode = run.exitCode;
        result.output = run.output;
        result.diagnostic = run.error;
        result.state = run.cancelled ? State::Cancelled : (run.started && !run.timedOut && run.exitCode == 0 ? State::Success : State::Failed);
        Inspect(prepared->root, result.after, {});
        return result;
    }
}
