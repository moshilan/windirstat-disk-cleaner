// WinDirStat - Windows Directory Statistics
// Copyright © WinDirStat Team
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.management.deployment.h>
#include <windows.storage.h>
#include <wrl/client.h>
#include "InstalledApps.h"
#include <algorithm>
#include <chrono>
#include <string_view>
#include <utility>

namespace Space::InstalledApps
{
    namespace
    {
        constexpr size_t FieldLimit = 4096;
        constexpr size_t EntryLimit = 25000;
        constexpr auto TimeLimit = std::chrono::seconds(15);
        constexpr wchar_t UninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
        constexpr std::array Sources{ Source::Machine32, Source::Machine64, Source::User32, Source::User64, Source::Msix };
        constexpr std::array SourceNames{ L"Machine32", L"Machine64", L"User32", L"User64", L"Msix" };
        using Microsoft::WRL::ComPtr;
        namespace AM = ABI::Windows::ApplicationModel;
        namespace MD = ABI::Windows::Management::Deployment;
        namespace FC = ABI::Windows::Foundation::Collections;

        int Compare(const std::wstring& a, const std::wstring& b)
        {
            return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                b.c_str(), static_cast<int>(b.size()), TRUE) - CSTR_EQUAL;
        }
        bool Contains(const std::wstring& value, const std::wstring& query)
        {
            if (query.size() > value.size()) return false;
            for (size_t i = 0; i <= value.size() - query.size(); ++i)
                if (::CompareStringOrdinal(value.data() + i, static_cast<int>(query.size()),
                    query.data(), static_cast<int>(query.size()), TRUE) == CSTR_EQUAL) return true;
            return false;
        }
        std::wstring Clean(std::wstring value)
        {
            for (auto& c : value) if (c < L' ' || c == 127) c = L' ';
            const auto begin = value.find_first_not_of(L' ');
            if (begin == std::wstring::npos) return {};
            return value.substr(begin, value.find_last_not_of(L' ') - begin + 1);
        }
        void Fail(SourceResult& source, const unsigned long error)
        {
            source.state = State::Partial;
            if (source.error == 0) source.error = error;
        }
        LoadResult EmptyResult()
        {
            LoadResult result;
            for (size_t i = 0; i < Sources.size(); ++i) result.sources[i].source = Sources[i];
            return result;
        }
        struct Key
        {
            HKEY value = nullptr;
            ~Key() { if (value != nullptr) ::RegCloseKey(value); }
        };
        struct Budget
        {
            size_t entries = 0;
            std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
            bool Exhausted() const { return entries >= EntryLimit || std::chrono::steady_clock::now() - started >= TimeLimit; }
        };
        bool Continue(LoadResult& result, SourceResult& source, const std::stop_token stop, const Budget& budget)
        {
            if (stop.stop_requested())
            {
                result.cancelled = true;
                source.state = State::Cancelled;
                source.error = ERROR_CANCELLED;
                return false;
            }
            if (budget.Exhausted()) { Fail(source, ERROR_TIMEOUT); return false; }
            return true;
        }
        bool Text(const HKEY key, const wchar_t* name, std::wstring& output, DWORD& error)
        {
            DWORD type = 0, bytes = 0;
            auto status = ::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
            if (status == ERROR_FILE_NOT_FOUND) return true;
            if (status != ERROR_SUCCESS) { error = status; return false; }
            if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes % sizeof(wchar_t) != 0 ||
                bytes > (FieldLimit + 1) * sizeof(wchar_t))
            { error = ERROR_INVALID_DATA; return false; }
            std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1);
            status = ::RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);
            if (status != ERROR_SUCCESS) { error = status; return false; }
            if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes % sizeof(wchar_t) != 0)
            { error = ERROR_INVALID_DATA; return false; }
            size_t length = bytes / sizeof(wchar_t);
            while (length != 0 && buffer[length - 1] == L'\0') --length;
            if (length > FieldLimit || std::find(buffer.begin(), buffer.begin() + length, L'\0') != buffer.begin() + length)
            { error = ERROR_INVALID_DATA; return false; }
            output.assign(buffer.data(), length);
            // REG_EXPAND_SZ remains literal metadata; no environment or directory resolution.
            return true;
        }
        void Registry(const HKEY hive, const std::wstring& path, const REGSAM view,
            SourceResult& source, LoadResult& result, Budget& budget, const std::stop_token stop)
        {
            if (!Continue(result, source, stop, budget)) return;
            Key root;
            auto status = ::RegOpenKeyExW(hive, path.c_str(), 0, KEY_ENUMERATE_SUB_KEYS | view, &root.value);
            if (status != ERROR_SUCCESS) { source.error = status; return; }
            source.state = State::Complete;
            for (DWORD index = 0; ; ++index)
            {
                if (!Continue(result, source, stop, budget)) return;
                wchar_t name[256]{};
                DWORD length = static_cast<DWORD>(std::size(name));
                status = ::RegEnumKeyExW(root.value, index, name, &length, nullptr, nullptr, nullptr, nullptr);
                if (status == ERROR_NO_MORE_ITEMS) return;
                ++budget.entries;
                if (status != ERROR_SUCCESS) { ++source.skipped; Fail(source, status); if (status != ERROR_MORE_DATA) return; continue; }
                Key entry;
                status = ::RegOpenKeyExW(root.value, name, 0, KEY_QUERY_VALUE | view, &entry.value);
                if (status != ERROR_SUCCESS) { ++source.skipped; Fail(source, status); continue; }
                Record record;
                record.source = source.source;
                record.key.assign(name, length);
                DWORD error = 0;
                for (const auto& field : { std::pair{ L"DisplayName", &record.name }, { L"Publisher", &record.publisher },
                    { L"DisplayVersion", &record.version }, { L"InstallLocation", &record.installLocation } })
                    if (!Text(entry.value, field.first, *field.second, error)) Fail(source, error);
                if (source.source != Source::Msix)
                {
                    DWORD type = 0, size = sizeof(DWORD), value = 0;
                    status = ::RegQueryValueExW(entry.value, L"EstimatedSize", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
                    if (status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(DWORD)) record.estimatedKiB = value;
                    else if (status != ERROR_FILE_NOT_FOUND) Fail(source, status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status);
                }
                if (auto item = Convert(record)) result.items.push_back(std::move(*item));
                else ++source.skipped;
            }
        }
        struct RuntimeApi
        {
            HMODULE module = nullptr;
            decltype(&::RoInitialize) initialize = nullptr;
            decltype(&::RoUninitialize) uninitialize = nullptr;
            decltype(&::RoGetActivationFactory) factory = nullptr;
            decltype(&::WindowsCreateString) create = nullptr;
            decltype(&::WindowsDeleteString) erase = nullptr;
            decltype(&::WindowsGetStringRawBuffer) buffer = nullptr;
            ~RuntimeApi() { if (module != nullptr) ::FreeLibrary(module); }
            bool Open()
            {
                wchar_t system[32768]{};
                const auto size = ::GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
                if (size == 0 || size >= std::size(system)) return false;
                module = ::LoadLibraryW((std::wstring(system, size) + L"\\combase.dll").c_str());
                if (module == nullptr) return false;
                initialize = reinterpret_cast<decltype(initialize)>(::GetProcAddress(module, "RoInitialize"));
                uninitialize = reinterpret_cast<decltype(uninitialize)>(::GetProcAddress(module, "RoUninitialize"));
                factory = reinterpret_cast<decltype(factory)>(::GetProcAddress(module, "RoGetActivationFactory"));
                create = reinterpret_cast<decltype(create)>(::GetProcAddress(module, "WindowsCreateString"));
                erase = reinterpret_cast<decltype(erase)>(::GetProcAddress(module, "WindowsDeleteString"));
                buffer = reinterpret_cast<decltype(buffer)>(::GetProcAddress(module, "WindowsGetStringRawBuffer"));
                if (!initialize || !uninitialize || !factory || !create || !erase || !buffer)
                { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
                return true;
            }
        };
        struct RuntimeThread
        {
            const RuntimeApi& api;
            ~RuntimeThread() { api.uninitialize(); }
        };
        struct RuntimeString
        {
            const RuntimeApi& api;
            HSTRING value = nullptr;
            ~RuntimeString() { if (value != nullptr) api.erase(value); }
            bool Read(std::wstring& output) const
            {
                UINT32 length = 0;
                const auto text = api.buffer(value, &length);
                if (length > FieldLimit || (length != 0 && text == nullptr)) return false;
                if (length != 0) output.assign(text, length);
                return output.find(L'\0') == std::wstring::npos;
            }
        };
        void Packages(SourceResult& source, LoadResult& result, Budget& budget, const std::stop_token stop)
        {
            if (!Continue(result, source, stop, budget)) return;
            RuntimeApi api;
            if (!api.Open()) { source.error = ::GetLastError(); return; }
            auto status = api.initialize(RO_INIT_MULTITHREADED);
            if (FAILED(status)) { source.error = static_cast<unsigned long>(status); return; }
            RuntimeThread thread{ api };
            RuntimeString className{ api };
            constexpr std::wstring_view name = L"Windows.Management.Deployment.PackageManager";
            status = api.create(name.data(), static_cast<UINT32>(name.size()), &className.value);
            ComPtr<IActivationFactory> factory;
            if (SUCCEEDED(status)) status = api.factory(className.value, __uuidof(IActivationFactory), reinterpret_cast<void**>(factory.GetAddressOf()));
            ComPtr<IInspectable> instance;
            if (SUCCEEDED(status)) status = factory->ActivateInstance(instance.GetAddressOf());
            ComPtr<MD::IPackageManager2> manager;
            if (SUCCEEDED(status)) status = instance.As(&manager);
            ComPtr<FC::IIterable<AM::Package*>> packages;
            if (SUCCEEDED(status)) status = manager->FindPackagesByUserSecurityIdWithPackageTypes(nullptr, MD::PackageTypes_Main, packages.GetAddressOf());
            ComPtr<FC::IIterator<AM::Package*>> iterator;
            if (SUCCEEDED(status)) status = packages->First(iterator.GetAddressOf());
            if (FAILED(status)) { source.error = static_cast<unsigned long>(status); return; }
            source.state = State::Complete;
            boolean current = false;
            status = iterator->get_HasCurrent(&current);
            while (SUCCEEDED(status) && current)
            {
                if (!Continue(result, source, stop, budget)) return;
                ++budget.entries;
                ComPtr<AM::IPackage> package;
                auto fieldStatus = iterator->get_Current(package.GetAddressOf());
                ComPtr<AM::IPackageId> id;
                if (SUCCEEDED(fieldStatus)) fieldStatus = package->get_Id(id.GetAddressOf());
                if (FAILED(fieldStatus)) { ++source.skipped; Fail(source, static_cast<unsigned long>(fieldStatus)); }
                else
                {
                    Record record;
                    record.source = Source::Msix;
                    const auto read = [&](const HRESULT hr, const RuntimeString& text, std::wstring& output)
                    {
                        if (FAILED(hr) || !text.Read(output)) Fail(source, FAILED(hr) ? static_cast<unsigned long>(hr) : ERROR_INVALID_DATA);
                    };
                    RuntimeString key{ api }, title{ api }, publisher{ api };
                    read(id->get_FullName(&key.value), key, record.key);
                    read(id->get_Name(&title.value), title, record.name);
                    read(id->get_Publisher(&publisher.value), publisher, record.publisher);
                    AM::PackageVersion version{};
                    if (const auto hr = id->get_Version(&version); SUCCEEDED(hr))
                        record.version = std::to_wstring(version.Major) + L"." + std::to_wstring(version.Minor) + L"." +
                            std::to_wstring(version.Build) + L"." + std::to_wstring(version.Revision);
                    else Fail(source, static_cast<unsigned long>(hr));
                    ComPtr<AM::IPackage2> display;
                    if (SUCCEEDED(package.As(&display)))
                    {
                        RuntimeString friendly{ api }, vendor{ api };
                        std::wstring value;
                        if (SUCCEEDED(display->get_DisplayName(&friendly.value)) && friendly.Read(value) && !Clean(value).empty()) record.name = value;
                        value.clear();
                        if (SUCCEEDED(display->get_PublisherDisplayName(&vendor.value)) && vendor.Read(value) && !Clean(value).empty()) record.publisher = value;
                    }
                    ComPtr<ABI::Windows::Storage::IStorageFolder> folder;
                    auto hr = package->get_InstalledLocation(folder.GetAddressOf());
                    ComPtr<ABI::Windows::Storage::IStorageItem> location;
                    if (SUCCEEDED(hr)) hr = folder.As(&location);
                    RuntimeString path{ api };
                    if (SUCCEEDED(hr)) hr = location->get_Path(&path.value);
                    read(hr, path, record.installLocation);
                    if (auto item = Convert(record)) result.items.push_back(std::move(*item));
                    else { ++source.skipped; Fail(source, ERROR_INVALID_DATA); }
                }
                status = iterator->MoveNext(&current);
            }
            if (FAILED(status)) Fail(source, static_cast<unsigned long>(status));
        }
        void Finish(LoadResult& result, const std::stop_token stop)
        {
            result.cancelled |= stop.stop_requested();
            if (result.cancelled)
                for (auto& source : result.sources)
                    if (source.state == State::Unavailable && source.error == 0)
                    { source.state = State::Cancelled; source.error = ERROR_CANCELLED; }
            Deduplicate(result.items);
        }
        bool FixtureName(const std::wstring& path)
        {
            constexpr std::wstring_view prefix = L"Software\\WinDirStatTests\\InstalledApps-";
            if (path.size() <= prefix.size() || Compare(path.substr(0, prefix.size()), std::wstring(prefix)) != 0) return false;
            const auto suffix = path.substr(prefix.size());
            if (suffix.size() != 32 && suffix.size() != 36) return false;
            for (size_t i = 0; i < suffix.size(); ++i)
            {
                const auto c = suffix[i];
                if (suffix.size() == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) { if (c != L'-') return false; }
                else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
            }
            return true;
        }
    }

    std::optional<Item> Convert(const Record& record)
    {
        if (std::ranges::find(Sources, record.source) == Sources.end() || record.key.empty() || record.key.size() > FieldLimit ||
            std::ranges::any_of(record.key, [](const wchar_t c) { return c < L' ' || c == 127; })) return std::nullopt;
        for (const auto* value : { &record.name, &record.publisher, &record.version, &record.installLocation })
            if (value->size() > FieldLimit || value->find(L'\0') != std::wstring::npos) return std::nullopt;
        Item item;
        item.source = record.source;
        item.name = Clean(record.name);
        if (item.name.empty()) return std::nullopt;
        item.publisher = Clean(record.publisher);
        item.version = Clean(record.version);
        // A displayed directory must not become a different path by replacing control characters.
        if (!std::ranges::any_of(record.installLocation, [](const wchar_t c) { return c < L' ' || c == 127; }))
            item.installLocation = Clean(record.installLocation);
        const auto prefix = record.source == Source::Machine32 ? L"machine32:" : record.source == Source::Machine64 ? L"machine64:" :
            record.source == Source::Msix ? L"msix:" : L"user:";
        item.id = prefix + record.key;
        if (record.source != Source::Msix && record.estimatedKiB && *record.estimatedKiB != 0)
            item.estimatedBytes = static_cast<std::uint64_t>(*record.estimatedKiB) * 1024;
        return item;
    }
    void Deduplicate(std::vector<Item>& items)
    {
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return Compare(a.id, b.id) < 0; });
        items.erase(std::unique(items.begin(), items.end(), [](const Item& a, const Item& b) { return Compare(a.id, b.id) == 0; }), items.end());
    }
    std::vector<size_t> Select(const std::vector<Item>& items, const std::wstring& query, const bool descending)
    {
        std::vector<size_t> selected;
        if (query.size() > FieldLimit || query.find(L'\0') != std::wstring::npos) return selected;
        const auto text = Clean(query);
        for (size_t i = 0; i < items.size(); ++i)
            if (text.empty() || Contains(items[i].name, text) || Contains(items[i].publisher, text) ||
                Contains(items[i].version, text) || Contains(items[i].installLocation, text)) selected.push_back(i);
        std::stable_sort(selected.begin(), selected.end(), [&](const size_t a, const size_t b)
        {
            const auto& left = items[a];
            const auto& right = items[b];
            if (left.estimatedBytes.has_value() != right.estimatedBytes.has_value()) return left.estimatedBytes.has_value();
            if (left.estimatedBytes && left.estimatedBytes != right.estimatedBytes)
                return descending ? *left.estimatedBytes > *right.estimatedBytes : *left.estimatedBytes < *right.estimatedBytes;
            if (const auto name = Compare(left.name, right.name); name != 0) return name < 0;
            if (const auto id = Compare(left.id, right.id); id != 0) return id < 0;
            return a < b;
        });
        return selected;
    }
    LoadResult Load(const std::stop_token stop)
    {
        auto result = EmptyResult();
        Budget budget;
        for (size_t i = 0; i < 4; ++i)
        {
            const bool machine = i < 2;
            const auto view = (i % 2 == 0) ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
            Registry(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, UninstallKey, view, result.sources[i], result, budget, stop);
        }
        Packages(result.sources[4], result, budget, stop);
        Finish(result, stop);
        return result;
    }
    LoadResult LoadFixtureRegistry(const std::wstring& hkcuTestRoot, const std::stop_token stop)
    {
        auto result = EmptyResult();
        result.invalidFixture = true;
        if (!FixtureName(hkcuTestRoot)) return result;
        Key root;
        const auto status = ::RegOpenKeyExW(HKEY_CURRENT_USER, hkcuTestRoot.c_str(), 0, KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS, &root.value);
        if (status != ERROR_SUCCESS) { for (auto& source : result.sources) source.error = status; return result; }
        std::wstring marker;
        DWORD error = 0;
        if (!Text(root.value, L"WDSInstalledAppsFixture", marker, error) || marker != L"v1") return result;
        result.invalidFixture = false;
        Budget budget;
        for (size_t i = 0; i < Sources.size(); ++i)
            Registry(root.value, SourceNames[i], 0, result.sources[i], result, budget, stop);
        Finish(result, stop);
        return result;
    }
}
