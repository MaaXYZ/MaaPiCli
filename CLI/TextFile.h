#pragma once

#include <array>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

#include "MaaUtils/ScopeLeave.hpp"

#ifdef _WIN32
#include "MaaUtils/SafeWindows.hpp"
#include <fcntl.h>
#include <io.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace MaaPiCliText
{
// The caller has canonicalized the path and checked its resource-directory boundary.
// Open every component without following replacement links, then read the checked handle.
inline std::optional<std::string> read_canonical_file(const std::filesystem::path& path)
{
    if (!path.is_absolute() || path != path.lexically_normal()) {
        return std::nullopt;
    }
#ifdef _WIN32
    auto anchor = path.root_path();
    // A UNC root must include the share, not just the server name.
    if (path.root_name().native().starts_with(L"\\\\")) {
        const auto relative = path.relative_path();
        if (relative.empty()) {
            return std::nullopt;
        }
        anchor /= *relative.begin();
    }
    HANDLE handle = CreateFileW(
        anchor.c_str(),
        FILE_READ_ATTRIBUTES | FILE_TRAVERSE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    OnScopeLeave([&]() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    });
    const auto matches_type = [](HANDLE opened, bool directory) {
        BY_HANDLE_FILE_INFORMATION info { };
        return GetFileType(opened) == FILE_TYPE_DISK && GetFileInformationByHandle(opened, &info)
               && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0
               && ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
    };
    if (!matches_type(handle, true)) {
        return std::nullopt;
    }
    const auto relative = path.lexically_relative(anchor);
    for (auto component = relative.begin(); component != relative.end(); ++component) {
        const bool directory = std::next(component) != relative.end();
        auto native_name = component->wstring();
        UNICODE_STRING name { };
        name.Length = static_cast<USHORT>(native_name.size() * sizeof(wchar_t));
        name.MaximumLength = name.Length;
        name.Buffer = native_name.data();
        OBJECT_ATTRIBUTES attributes { };
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = handle;
        attributes.ObjectName = &name;
        attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
        IO_STATUS_BLOCK status { };
        HANDLE next = INVALID_HANDLE_VALUE;
        constexpr ULONG kOpen = 1;
        constexpr ULONG kDirectory = 0x1;
        constexpr ULONG kNonDirectory = 0x40;
        constexpr ULONG kSynchronous = 0x20;
        constexpr ULONG kOpenReparsePoint = 0x00200000;
        // Each name is a single component relative to the pinned directory handle.
        if (NtCreateFile(
                &next,
                (directory ? FILE_READ_ATTRIBUTES | FILE_TRAVERSE : GENERIC_READ) | SYNCHRONIZE,
                &attributes,
                &status,
                nullptr,
                FILE_ATTRIBUTE_NORMAL,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                kOpen,
                kOpenReparsePoint | kSynchronous | (directory ? kDirectory : kNonDirectory),
                nullptr,
                0)
            < 0) {
            return std::nullopt;
        }
        CloseHandle(handle);
        handle = next;
        if (!matches_type(handle, directory)) {
            return std::nullopt;
        }
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_TEXT);
    if (descriptor < 0) {
        return std::nullopt;
    }
    handle = INVALID_HANDLE_VALUE; // Ownership passed to the CRT descriptor.
    FILE* stream = _fdopen(descriptor, "r");
    if (!stream) {
        _close(descriptor);
        return std::nullopt;
    }
#else
    int descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return std::nullopt;
    }
    const auto relative = path.relative_path();
    for (auto component = relative.begin(); component != relative.end(); ++component) {
        const bool directory = std::next(component) != relative.end();
        const int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : O_NONBLOCK);
        const int next = ::openat(descriptor, component->c_str(), flags);
        ::close(descriptor);
        if (next < 0) {
            return std::nullopt;
        }
        descriptor = next;
    }
    struct stat info { };
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(descriptor);
        return std::nullopt;
    }
    FILE* stream = ::fdopen(descriptor, "r");
    if (!stream) {
        ::close(descriptor);
        return std::nullopt;
    }
#endif
    OnScopeLeave([&]() { std::fclose(stream); });
    std::string content;
    std::array<char, 4096> buffer;
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), stream)) {
        content.append(buffer.data(), count);
    }
    return std::ferror(stream) ? std::nullopt : std::optional(std::move(content));
}
}
