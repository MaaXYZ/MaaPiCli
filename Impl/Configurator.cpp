#include "ProjectInterface/Configurator.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include "MaaUtils/SafeWindows.hpp"

#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "MaaFramework/Utility/MaaUtility.h"
#include "MaaUtils/Logger.h"
#include "MaaUtils/Platform.h"
#include "MaaUtils/ScopeLeave.hpp"
#include "MaaUtils/StringMisc.hpp"
#include "ProjectInterface/Parser.h"
#include "SecretStore.h"

MAA_PROJECT_INTERFACE_NS_BEGIN

namespace
{
#if defined(__APPLE__)
constexpr auto kConfigAclType = ACL_TYPE_EXTENDED;
#endif

#if defined(__APPLE__)
bool missing_acl_error(int error)
{
    if (error == EOPNOTSUPP || error == ENOTSUP) {
        return true;
    }
#ifdef ENOATTR
    if (error == ENOATTR) {
        return true;
    }
#endif
#ifdef __APPLE__
    // macOS reports an absent extended ACL as ENOENT even when the file exists.
    return error == ENOENT;
#else
    return false;
#endif
}
#endif

#if defined(__linux__)
constexpr char kPosixAclXattr[] = "system.posix_acl_access";

bool missing_xattr_error(int error)
{
    if (error == ENOTSUP || error == EOPNOTSUPP) {
        return true;
    }
#if defined(ENOENT)
    if (error == ENOENT) {
        return true;
    }
#endif
#if defined(ENODATA)
    if (error == ENODATA) {
        return true;
    }
#endif
#if defined(ENOATTR) && ENOATTR != ENODATA
    if (error == ENOATTR) {
        return true;
    }
#endif
    return false;
}

bool read_configuration_acl(int descriptor, std::string& acl)
{
    const auto size = ::fgetxattr(descriptor, kPosixAclXattr, nullptr, 0);
    if (size < 0) {
        return missing_xattr_error(errno);
    }
    if (size == 0) {
        errno = EINVAL;
        return false;
    }

    acl.resize(static_cast<size_t>(size), '\0');
    const auto actual_size = ::fgetxattr(descriptor, kPosixAclXattr, acl.data(), acl.size());
    if (actual_size < 0) {
        return false;
    }
    if (actual_size != static_cast<ssize_t>(acl.size())) {
        errno = ERANGE;
        return false;
    }
    return true;
}
#endif

#ifdef _WIN32
using NTSTATUS = LONG;

struct NtUnicodeString
{
    USHORT length = 0;
    USHORT maximum_length = 0;
    PWSTR buffer = nullptr;
};

struct NtObjectAttributes
{
    ULONG length = 0;
    HANDLE root_directory = nullptr;
    NtUnicodeString* object_name = nullptr;
    ULONG attributes = 0;
    PVOID security_descriptor = nullptr;
    PVOID security_quality_of_service = nullptr;
};

struct NtIoStatusBlock
{
    union
    {
        NTSTATUS status = 0;
        PVOID pointer;
    };

    ULONG_PTR information = 0;
};

constexpr ULONG kNtCaseInsensitive = 0x00000040;
constexpr ULONG kNtFileDirectoryFile = 0x00000001;
constexpr ULONG kNtFileOpenReparsePoint = 0x00200000;
constexpr ULONG kNtFileNonDirectoryFile = 0x00000040;
constexpr ULONG kNtFileSynchronousIoNonalert = 0x00000020;
constexpr ULONG kNtFileOpen = 0x00000001;
constexpr ULONG kNtFileCreate = 0x00000002;
constexpr ULONG kNtFileOpenIf = 0x00000003;
constexpr ULONG kNtFileRenameInformationEx = 65;
constexpr ULONG kFileRenameFlagReplaceIfExists = 0x00000001;
constexpr ULONG kFileRenameFlagPosixSemantics = 0x00000002;
constexpr NTSTATUS kNtObjectNameNotFound = static_cast<NTSTATUS>(0xC0000034u);
constexpr NTSTATUS kNtObjectPathNotFound = static_cast<NTSTATUS>(0xC000003Au);
constexpr NTSTATUS kNtPrivilegeNotHeld = static_cast<NTSTATUS>(0xC0000061u);

struct FileRenameRequest
{
    ULONG flags = 0;
    HANDLE root_directory = nullptr;
    DWORD file_name_length = 0;
    wchar_t file_name[1] = { };
};

extern "C" NTSTATUS WINAPI NtCreateFile(
    HANDLE* file_handle,
    ACCESS_MASK desired_access,
    NtObjectAttributes* object_attributes,
    NtIoStatusBlock* io_status_block,
    LARGE_INTEGER* allocation_size,
    ULONG file_attributes,
    ULONG share_access,
    ULONG create_disposition,
    ULONG create_options,
    PVOID ea_buffer,
    ULONG ea_length);

extern "C" NTSTATUS WINAPI NtDeleteFile(NtObjectAttributes* object_attributes);

extern "C" NTSTATUS WINAPI NtSetInformationFile(
    HANDLE file_handle,
    NtIoStatusBlock* io_status_block,
    PVOID file_information,
    ULONG length,
    ULONG file_information_class);

NtUnicodeString nt_name(const std::wstring& name)
{
    NtUnicodeString result;
    result.length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    result.maximum_length = result.length;
    result.buffer = const_cast<PWSTR>(name.c_str());
    return result;
}

NtObjectAttributes nt_relative_attributes(HANDLE root, NtUnicodeString& name)
{
    NtObjectAttributes attributes { };
    attributes.length = sizeof(attributes);
    attributes.root_directory = root;
    attributes.object_name = &name;
    attributes.attributes = kNtCaseInsensitive;
    return attributes;
}

bool nt_success(NTSTATUS status)
{
    return status >= 0;
}

bool nt_open_relative(
    HANDLE root,
    const std::filesystem::path& name,
    ACCESS_MASK desired_access,
    ULONG create_disposition,
    ULONG create_options,
    HANDLE* handle,
    NTSTATUS* status_out = nullptr)
{
    const auto native_name = name.wstring();
    NtUnicodeString object_name = nt_name(native_name);
    NtObjectAttributes attributes = nt_relative_attributes(root, object_name);
    NtIoStatusBlock io_status { };
    const NTSTATUS status = NtCreateFile(
        handle,
        desired_access,
        &attributes,
        &io_status,
        nullptr,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        create_disposition,
        create_options,
        nullptr,
        0);
    if (status_out != nullptr) {
        *status_out = status;
    }
    return nt_success(status);
}
#endif

MaaWin32ScreencapMethod parse_win32_screencap_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaWin32ScreencapMethod> mapping = {
        { "GDI", MaaWin32ScreencapMethod_GDI },
        { "FramePool", MaaWin32ScreencapMethod_FramePool },
        { "DXGI_DesktopDup", MaaWin32ScreencapMethod_DXGI_DesktopDup },
        { "DXGI_DesktopDup_Window", MaaWin32ScreencapMethod_DXGI_DesktopDup_Window },
        { "PrintWindow", MaaWin32ScreencapMethod_PrintWindow },
        { "ScreenDC", MaaWin32ScreencapMethod_ScreenDC },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaWin32ScreencapMethod_None;
}

MaaWin32InputMethod parse_win32_input_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaWin32InputMethod> mapping = {
        { "Seize", MaaWin32InputMethod_Seize },
        { "SendMessage", MaaWin32InputMethod_SendMessage },
        { "PostMessage", MaaWin32InputMethod_PostMessage },
        { "LegacyEvent", MaaWin32InputMethod_LegacyEvent },
        { "PostThreadMessage", MaaWin32InputMethod_PostThreadMessage },
        { "SendMessageWithCursorPos", MaaWin32InputMethod_SendMessageWithCursorPos },
        { "PostMessageWithCursorPos", MaaWin32InputMethod_PostMessageWithCursorPos },
        { "SendMessageWithWindowPos", MaaWin32InputMethod_SendMessageWithWindowPos },
        { "PostMessageWithWindowPos", MaaWin32InputMethod_PostMessageWithWindowPos },
        { "Interception", MaaWin32InputMethod_Interception },
        { "AnchoredTouch", MaaWin32InputMethod_AnchoredTouch },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaWin32InputMethod_None;
}

MaaGamepadType parse_gamepad_type(const std::string& type)
{
    static const std::unordered_map<std::string, MaaGamepadType> mapping = {
        { "Xbox360", MaaGamepadType_Xbox360 },
        { "DualShock4", MaaGamepadType_DualShock4 },
        { "DS4", MaaGamepadType_DualShock4 },
    };

    if (auto it = mapping.find(type); it != mapping.end()) {
        return it->second;
    }
    return MaaGamepadType_Xbox360;
}

MaaMacOSScreencapMethod parse_macos_screencap_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaMacOSScreencapMethod> mapping = {
        { "ScreenCaptureKit", MaaMacOSScreencapMethod_ScreenCaptureKit },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaMacOSScreencapMethod_None;
}

MaaMacOSInputMethod parse_macos_input_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaMacOSInputMethod> mapping = {
        { "GlobalEvent", MaaMacOSInputMethod_GlobalEvent },
        { "PostToPid", MaaMacOSInputMethod_PostToPid },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaMacOSInputMethod_None;
}

MaaLinuxScreencapMethod parse_linux_screencap_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaLinuxScreencapMethod> mapping = {
        { "Wlr", MaaLinuxScreencapMethod_Wlr },
        { "PipeWire", MaaLinuxScreencapMethod_PipeWire },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaLinuxScreencapMethod_None;
}

MaaLinuxInputMethod parse_linux_input_method(const std::string& method)
{
    static const std::unordered_map<std::string, MaaLinuxInputMethod> mapping = {
        { "Wlr", MaaLinuxInputMethod_Wlr },
        { "UInput", MaaLinuxInputMethod_UInput },
        { "Libei", MaaLinuxInputMethod_Libei },
    };

    if (auto it = mapping.find(method); it != mapping.end()) {
        return it->second;
    }
    return MaaLinuxInputMethod_None;
}

std::string pretask_identifier(const InterfaceData::Pretask& pretask)
{
    return pretask.name.empty() ? pretask.exec : pretask.name;
}

template <typename Transform>
bool transform_stored_passwords(
    const std::unordered_map<std::string, InterfaceData::Option>& options,
    Configuration& config,
    Transform transform)
{
    auto transform_option_list = [&](const std::string& scope, std::vector<Configuration::Option>& config_options) {
        for (auto& config_option : config_options) {
            auto option_iter = options.find(config_option.name);
            if (option_iter == options.end() || option_iter->second.type != InterfaceData::Option::Type::Input) {
                continue;
            }

            for (const auto& input_def : option_iter->second.inputs) {
                if (!input_def.password) {
                    continue;
                }

                auto value_iter = config_option.inputs.find(input_def.name);
                if (value_iter == config_option.inputs.end()) {
                    continue;
                }

                auto value = transform(scope + "/" + config_option.name + "/" + input_def.name, value_iter->second);
                if (value.empty() && !value_iter->second.empty()) {
                    return false;
                }
                value_iter->second = std::move(value);
            }
        }
        return true;
    };

    if (!transform_option_list("global", config.global_option) || !transform_option_list("resource", config.resource_option)
        || !transform_option_list("controller", config.controller_option)) {
        return false;
    }

    for (auto& task : config.task) {
        if (!transform_option_list("task/" + task.name, task.option)) {
            return false;
        }
    }

    for (auto& pretask : config.pretask) {
        if (!transform_option_list("pretask/" + pretask.name, pretask.option)) {
            return false;
        }
    }

    return true;
}

bool path_is_within_directory(const std::filesystem::path& path, const std::filesystem::path& directory)
{
    const auto relative = path.lexically_relative(directory);
    if (relative.empty() || relative == std::filesystem::path(".")) {
        return false;
    }
    for (const auto& component : relative) {
        if (component == std::filesystem::path("..")) {
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
std::optional<std::filesystem::path> windows_full_path(const std::filesystem::path& path)
{
    const auto input = path.wstring();
    const auto length = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (length == 0) {
        return std::nullopt;
    }
    std::wstring result(length, L'\0');
    if (GetFullPathNameW(input.c_str(), length, result.data(), nullptr) != length - 1) {
        return std::nullopt;
    }
    result.pop_back();
    return std::filesystem::path(result);
}

std::optional<std::filesystem::path> opened_final_path(HANDLE handle)
{
    const auto length = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    if (length == 0) {
        return std::nullopt;
    }
    std::wstring result(length, L'\0');
    if (GetFinalPathNameByHandleW(handle, result.data(), length, FILE_NAME_NORMALIZED) != length - 1) {
        return std::nullopt;
    }
    result.pop_back();
    if (result.starts_with(L"\\\\?\\UNC\\")) {
        result.replace(0, 8, L"\\\\");
    }
    else if (result.starts_with(L"\\\\?\\")) {
        result.erase(0, 4);
    }
    return std::filesystem::path(result).lexically_normal();
}

bool file_information_matches(const BY_HANDLE_FILE_INFORMATION& left, const BY_HANDLE_FILE_INFORMATION& right)
{
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber && left.nFileIndexHigh == right.nFileIndexHigh
           && left.nFileIndexLow == right.nFileIndexLow;
}

bool handle_refers_to_path(HANDLE handle, const std::filesystem::path& expected)
{
    BY_HANDLE_FILE_INFORMATION expected_information { };
    HANDLE expected_handle = CreateFileW(
        expected.c_str(),
        FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (expected_handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(expected_handle, &expected_information)) {
        if (expected_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(expected_handle);
        }
        return false;
    }
    CloseHandle(expected_handle);

    BY_HANDLE_FILE_INFORMATION information { };
    return GetFileInformationByHandle(handle, &information) && file_information_matches(information, expected_information);
}

bool handle_matches_expected_path(HANDLE handle, const std::filesystem::path& expected)
{
    BY_HANDLE_FILE_INFORMATION information { };
    if (!GetFileInformationByHandle(handle, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    return handle_refers_to_path(handle, expected);
}

bool handle_is_in_directory(HANDLE handle, HANDLE directory)
{
    BY_HANDLE_FILE_INFORMATION information { };
    if (!GetFileInformationByHandle(handle, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    const auto final_path = opened_final_path(handle);
    return final_path.has_value() && !final_path->parent_path().empty() && handle_refers_to_path(directory, final_path->parent_path());
}
#endif

class TrustedConfigurationDirectory
{
public:
    static std::optional<TrustedConfigurationDirectory> acquire(const std::filesystem::path& directory, bool create_missing)
    {
#ifdef _WIN32
        if (create_missing) {
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) {
                LogError << "Failed to create configuration directory" << VAR(directory) << VAR(error.message());
                return std::nullopt;
            }
        }

        std::error_code error;
        auto canonical_directory = std::filesystem::weakly_canonical(directory, error);
        if (error) {
            LogError << "Failed to resolve configuration directory" << VAR(directory) << VAR(error.message());
            return std::nullopt;
        }
        HANDLE handle = CreateFileW(
            canonical_directory.c_str(),
            FILE_READ_ATTRIBUTES | FILE_TRAVERSE | FILE_DELETE_CHILD | SYNCHRONIZE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            LogError << "Failed to open configuration directory" << VAR(canonical_directory) << VAR(GetLastError());
            return std::nullopt;
        }

        if (!handle_refers_to_path(handle, canonical_directory)) {
            LogError << "Configuration directory changed while opening" << VAR(canonical_directory);
            CloseHandle(handle);
            return std::nullopt;
        }

        BY_HANDLE_FILE_INFORMATION information { };
        const auto final_path = GetFileInformationByHandle(handle, &information) ? opened_final_path(handle) : std::nullopt;
        if (!final_path || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            LogError << "Failed to resolve opened configuration directory" << VAR(canonical_directory) << VAR(GetLastError());
            CloseHandle(handle);
            return std::nullopt;
        }
        return TrustedConfigurationDirectory(handle, *final_path);
#else
        std::error_code error;
        auto canonical_directory = std::filesystem::weakly_canonical(directory, error);
        if (error) {
            LogError << "Failed to resolve configuration directory" << VAR(directory) << VAR(error.message());
            return std::nullopt;
        }

        int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (current < 0) {
            LogError << "Failed to open filesystem root" << VAR(errno);
            return std::nullopt;
        }

        std::filesystem::path current_path("/");
        for (const auto& component : canonical_directory) {
            if (component.empty() || component == ".") {
                continue;
            }
            int next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0 && errno == ENOENT && create_missing) {
                if (::mkdirat(current, component.c_str(), 0755) != 0 && errno != EEXIST) {
                    LogError << "Failed to create configuration directory" << VAR(current_path / component) << VAR(errno);
                    ::close(current);
                    return std::nullopt;
                }
                next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            }
            if (next < 0) {
                LogError << "Failed to open trusted configuration directory" << VAR(current_path / component) << VAR(errno);
                ::close(current);
                return std::nullopt;
            }

            ::close(current);
            current = next;
            current_path /= component;
        }

        if (current_path != canonical_directory) {
            LogError << "Failed to open trusted configuration directory" << VAR(canonical_directory);
            ::close(current);
            return std::nullopt;
        }
        return TrustedConfigurationDirectory(current, canonical_directory);
#endif
    }

    static std::optional<TrustedConfigurationDirectory>
        descend(const TrustedConfigurationDirectory& root, const std::filesystem::path& relative_directory)
    {
        if (relative_directory.empty() || relative_directory.lexically_normal() == ".") {
            return acquire(root.path(), false);
        }
        std::filesystem::path current_path = root.path();
        if (!relative_directory.empty() && relative_directory != ".") {
            current_path /= relative_directory;
        }

#ifdef _WIN32
        HANDLE current = root.handle();
        bool owns_current = false;
        for (const auto& component : relative_directory) {
            if (component.empty() || component == ".") {
                continue;
            }
            HANDLE next = INVALID_HANDLE_VALUE;
            if (!nt_open_relative(
                    current,
                    component,
                    FILE_READ_ATTRIBUTES | FILE_TRAVERSE | FILE_DELETE_CHILD | SYNCHRONIZE,
                    kNtFileOpen,
                    kNtFileDirectoryFile | kNtFileOpenReparsePoint,
                    &next)) {
                LogError << "Failed to open trusted configuration directory" << VAR(current_path / component);
                if (owns_current) {
                    CloseHandle(current);
                }
                return std::nullopt;
            }
            if (owns_current) {
                CloseHandle(current);
            }
            current = next;
            owns_current = true;
        }

        const auto expected_path = windows_full_path(current_path);
        if (!expected_path || !handle_matches_expected_path(current, *expected_path)) {
            LogError << "Configuration directory changed while opening" << VAR(current_path);
            if (owns_current) {
                CloseHandle(current);
            }
            return std::nullopt;
        }
        return TrustedConfigurationDirectory(current, current_path);
#else
        int current = root.descriptor();
        bool owns_current = false;
        for (const auto& component : relative_directory) {
            if (component.empty() || component == ".") {
                continue;
            }
            const int next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) {
                LogError << "Failed to open trusted configuration directory" << VAR(current_path / component) << VAR(errno);
                if (owns_current) {
                    ::close(current);
                }
                return std::nullopt;
            }
            if (owns_current) {
                ::close(current);
            }
            current = next;
            owns_current = true;
        }
        return TrustedConfigurationDirectory(current, current_path);
#endif
    }

    TrustedConfigurationDirectory(TrustedConfigurationDirectory&& other) noexcept
        : path_(std::move(other.path_))
#ifdef _WIN32
        , handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE))
#else
        , descriptor_(std::exchange(other.descriptor_, -1))
#endif
    {
    }

    TrustedConfigurationDirectory& operator=(TrustedConfigurationDirectory&&) = delete;

    ~TrustedConfigurationDirectory()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
#endif
    }

    const std::filesystem::path& path() const { return path_; }

#ifdef _WIN32
    HANDLE handle() const { return handle_; }
#else
    int descriptor() const { return descriptor_; }
#endif

private:
    TrustedConfigurationDirectory(
#ifdef _WIN32
        HANDLE handle,
#else
        int descriptor,
#endif
        std::filesystem::path path)
        : path_(std::move(path))
#ifdef _WIN32
        , handle_(handle)
#else
        , descriptor_(descriptor)
#endif
    {
    }

    std::filesystem::path path_;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

class TrustedConfigurationTarget
{
public:
    TrustedConfigurationTarget(TrustedConfigurationDirectory directory, std::filesystem::path file_name)
        : directory_(std::move(directory))
        , file_name_(std::move(file_name))
    {
    }

    const TrustedConfigurationDirectory& directory() const { return directory_; }

    const std::filesystem::path& file_name() const { return file_name_; }

private:
    TrustedConfigurationDirectory directory_;
    std::filesystem::path file_name_;
};

std::optional<TrustedConfigurationTarget> resolve_configuration_target(const std::filesystem::path& config_path)
{
    auto directory = TrustedConfigurationDirectory::acquire(config_path.parent_path(), true);
    if (!directory) {
        return std::nullopt;
    }

#ifdef _WIN32
    const auto target_path = windows_full_path(config_path);
    if (!target_path) {
        LogError << "Failed to resolve configuration target" << VAR(config_path) << VAR(GetLastError());
        return std::nullopt;
    }
    const auto requested_directory = target_path->parent_path();
    if (!path_is_within_directory(*target_path, requested_directory)) {
        LogError << "Configuration target is outside its configuration directory" << VAR(config_path) << VAR(*target_path);
        return std::nullopt;
    }
    const auto relative_directory = target_path->parent_path().lexically_relative(requested_directory);
#else
    std::error_code error;
    const auto target_path = std::filesystem::weakly_canonical(config_path, error);
    if (error) {
        LogError << "Failed to resolve configuration target" << VAR(config_path) << VAR(error.message());
        return std::nullopt;
    }
#endif
#ifndef _WIN32
    if (!path_is_within_directory(target_path, directory->path())) {
        LogError << "Configuration target is outside its configuration directory" << VAR(config_path) << VAR(target_path);
        return std::nullopt;
    }
#endif

#ifdef _WIN32
    auto target_directory = TrustedConfigurationDirectory::descend(*directory, relative_directory);
#else
    auto target_directory =
        TrustedConfigurationDirectory::descend(*directory, target_path.parent_path().lexically_relative(directory->path()));
#endif
    if (!target_directory) {
        return std::nullopt;
    }

#ifdef _WIN32
    return TrustedConfigurationTarget(std::move(*target_directory), target_path->filename());
#else
    return TrustedConfigurationTarget(std::move(*target_directory), target_path.filename());
#endif
}

class ConfigFileLock
{
public:
    explicit ConfigFileLock(const TrustedConfigurationTarget& target)
    {
        const std::filesystem::path lock_name = ".maa_pi_config.lock";

#ifdef _WIN32
        if (!nt_open_relative(
                target.directory().handle(),
                lock_name.wstring(),
                GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                kNtFileOpenIf,
                kNtFileOpenReparsePoint | kNtFileNonDirectoryFile | kNtFileSynchronousIoNonalert,
                &handle_)) {
            LogError << "Failed to open configuration lock" << VAR(target.directory().path() / lock_name);
            handle_ = INVALID_HANDLE_VALUE;
            return;
        }
        if (!handle_is_in_directory(handle_, target.directory().handle())) {
            LogError << "Configuration lock is not in the trusted configuration directory" << VAR(lock_name);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return;
        }

        OVERLAPPED overlapped { };
        if (!LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
            LogError << "Failed to lock configuration" << VAR(lock_name) << VAR(GetLastError());
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        descriptor_ = ::openat(target.directory().descriptor(), lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (descriptor_ < 0) {
            LogError << "Failed to open configuration lock" << VAR(lock_name) << VAR(errno);
            return;
        }

        struct stat lock_status { };
        if (::fstat(descriptor_, &lock_status) != 0 || !S_ISREG(lock_status.st_mode)) {
            LogError << "Configuration lock is not a regular file" << VAR(lock_name) << VAR(errno);
            ::close(descriptor_);
            descriptor_ = -1;
            return;
        }

        if (::flock(descriptor_, LOCK_EX) != 0) {
            LogError << "Failed to lock configuration" << VAR(lock_name) << VAR(errno);
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
    }

    ~ConfigFileLock()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED overlapped { };
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
            CloseHandle(handle_);
        }
#else
        if (descriptor_ >= 0) {
            ::flock(descriptor_, LOCK_UN);
            ::close(descriptor_);
        }
#endif
    }

    bool valid() const
    {
#ifdef _WIN32
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return descriptor_ >= 0;
#endif
    }

private:
    ConfigFileLock(const ConfigFileLock&) = delete;
    ConfigFileLock& operator=(const ConfigFileLock&) = delete;

#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

class ExistingConfigurationFile
{
public:
    explicit ExistingConfigurationFile(const TrustedConfigurationTarget& target)
        : file_name_(target.file_name())
    {
#ifdef _WIN32
        NTSTATUS status = 0;
        const ACCESS_MASK base_access = GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        if (!nt_open_relative(
                target.directory().handle(),
                file_name_.wstring(),
                base_access | ACCESS_SYSTEM_SECURITY,
                kNtFileOpen,
                kNtFileOpenReparsePoint | kNtFileNonDirectoryFile | kNtFileSynchronousIoNonalert,
                &handle_,
                &status)) {
            if (status == kNtObjectNameNotFound || status == kNtObjectPathNotFound) {
                exists_ = false;
                valid_ = true;
                return;
            }

            if (status == kNtPrivilegeNotHeld) {
                if (!nt_open_relative(
                        target.directory().handle(),
                        file_name_.wstring(),
                        base_access,
                        kNtFileOpen,
                        kNtFileOpenReparsePoint | kNtFileNonDirectoryFile | kNtFileSynchronousIoNonalert,
                        &handle_,
                        &status)) {
                    if (status == kNtObjectNameNotFound || status == kNtObjectPathNotFound) {
                        exists_ = false;
                        valid_ = true;
                    }
                    else {
                        LogError << "Failed to open existing configuration" << VAR(file_name_) << VAR(status);
                    }
                    return;
                }
            }
            else {
                LogError << "Failed to open existing configuration" << VAR(file_name_) << VAR(status);
                return;
            }
        }
        else {
            security_information_ |= SACL_SECURITY_INFORMATION;
        }

        BY_HANDLE_FILE_INFORMATION information { };
        if (!GetFileInformationByHandle(handle_, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
            || !handle_is_in_directory(handle_, target.directory().handle())) {
            LogError << "Configuration target is not a trusted regular file" << VAR(file_name_);
            return;
        }

        exists_ = true;
        auto result = GetSecurityInfo(
            handle_,
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | security_information_,
            &owner_,
            &group_,
            &dacl_,
            &sacl_,
            &security_descriptor_);
        if (result != ERROR_SUCCESS || security_descriptor_ == nullptr) {
            LogError << "Failed to read configuration security information" << VAR(file_name_) << VAR(result);
            return;
        }

        if ((security_information_ & SACL_SECURITY_INFORMATION) == 0) {
            SECURITY_DESCRIPTOR_CONTROL descriptor_control = 0;
            DWORD revision = 0;
            if (!GetSecurityDescriptorControl(security_descriptor_, &descriptor_control, &revision)
                || (descriptor_control & SE_SACL_PRESENT) != 0) {
                LogError << "Failed to verify configuration SACL" << VAR(file_name_) << VAR(GetLastError());
                return;
            }
        }

        security_information_ |= OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
        if (sacl_ == nullptr) {
            security_information_ &= ~SACL_SECURITY_INFORMATION;
        }
        valid_ = true;
#else
        descriptor_ = ::openat(target.directory().descriptor(), file_name_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor_ < 0) {
            if (errno == ENOENT) {
                exists_ = false;
                valid_ = true;
            }
            else {
                LogError << "Failed to open existing configuration" << VAR(file_name_) << VAR(errno);
            }
            return;
        }

        if (::fstat(descriptor_, &status_) != 0 || !S_ISREG(status_.st_mode)) {
            LogError << "Configuration target is not a regular file" << VAR(file_name_) << VAR(errno);
            return;
        }
        exists_ = true;

#if defined(__APPLE__)
        errno = 0;
        acl_ = ::acl_get_fd(descriptor_);
        if (acl_ == nullptr && !missing_acl_error(errno)) {
            LogError << "Failed to read configuration ACL" << VAR(file_name_) << VAR(errno);
            return;
        }
#elif defined(__linux__)
        if (!read_configuration_acl(descriptor_, acl_)) {
            LogError << "Failed to read configuration ACL" << VAR(file_name_) << VAR(errno);
            return;
        }
#endif
        valid_ = true;
#endif
    }

    ExistingConfigurationFile(const ExistingConfigurationFile&) = delete;
    ExistingConfigurationFile& operator=(const ExistingConfigurationFile&) = delete;

    ~ExistingConfigurationFile()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        if (security_descriptor_ != nullptr) {
            LocalFree(security_descriptor_);
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
#if defined(__APPLE__)
        if (acl_ != nullptr) {
            ::acl_free(acl_);
        }
#endif
#endif
    }

    bool valid() const { return valid_; }

    bool exists() const { return exists_; }

    bool read_all(std::string& content)
    {
        if (!exists_) {
            return false;
        }

        content.clear();
        std::array<char, 64 * 1024> buffer { };
        for (;;) {
#ifdef _WIN32
            DWORD read_size = 0;
            if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read_size, nullptr)) {
                LogError << "Failed to read existing configuration" << VAR(file_name_) << VAR(GetLastError());
                return false;
            }
#else
            const auto read_size = ::read(descriptor_, buffer.data(), buffer.size());
            if (read_size < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LogError << "Failed to read existing configuration" << VAR(file_name_) << VAR(errno);
                return false;
            }
            if (read_size == 0) {
                break;
            }
            content.append(buffer.data(), static_cast<size_t>(read_size));
            continue;
#endif
            if (read_size == 0) {
                break;
            }
            content.append(buffer.data(), read_size);
        }
        return true;
    }

    std::optional<Configuration> parse_config()
    {
        std::string content;
        if (!read_all(content)) {
            return std::nullopt;
        }
        auto json_opt = json::parse(content);
        if (!json_opt) {
            LogError << "Failed to parse existing configuration" << VAR(file_name_);
            return std::nullopt;
        }
        return Parser::parse_config(*json_opt);
    }

#ifdef _WIN32
    SECURITY_INFORMATION security_information() const { return security_information_; }

    PSID owner() const { return owner_; }

    PSID group() const { return group_; }

    PACL dacl() const { return dacl_; }

    PACL sacl() const { return sacl_; }
#else
    const struct stat& status() const { return status_; }

#if defined(__APPLE__)
    acl_t acl() const { return acl_; }
#elif defined(__linux__)
    const std::string& acl() const { return acl_; }
#endif
#endif

private:
    std::filesystem::path file_name_;
    bool exists_ = false;
    bool valid_ = false;
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    SECURITY_INFORMATION security_information_ = 0;
    PSECURITY_DESCRIPTOR security_descriptor_ = nullptr;
    PSID owner_ = nullptr;
    PSID group_ = nullptr;
    PACL dacl_ = nullptr;
    PACL sacl_ = nullptr;
#else
    int descriptor_ = -1;
    struct stat status_ { };
#if defined(__APPLE__)
    acl_t acl_ = nullptr;
#elif defined(__linux__)
    std::string acl_;
#endif
#endif
};

class TemporaryConfigFile
{
public:
    TemporaryConfigFile(const TrustedConfigurationTarget& target, const ExistingConfigurationFile& existing)
        : target_name_(target.file_name())
        , temporary_name_(target.file_name())
    {
#ifdef _WIN32
        directory_descriptor_ = target.directory().handle();
#else
        directory_descriptor_ = target.directory().descriptor();
#endif
        temporary_name_ += ".tmp";
        remove_from_trusted_directory(directory_descriptor_, temporary_name_);

#ifdef _WIN32
        NTSTATUS status = 0;
        ACCESS_MASK desired_access = DELETE | FILE_READ_ATTRIBUTES | GENERIC_WRITE | WRITE_OWNER | WRITE_DAC | SYNCHRONIZE;
        if (existing.exists() && (existing.security_information() & SACL_SECURITY_INFORMATION) != 0) {
            desired_access |= ACCESS_SYSTEM_SECURITY;
        }
        if (!nt_open_relative(
                target.directory().handle(),
                temporary_name_,
                desired_access,
                kNtFileCreate,
                kNtFileOpenReparsePoint | kNtFileNonDirectoryFile | kNtFileSynchronousIoNonalert,
                &handle_,
                &status)) {
            LogError << "Failed to create temporary configuration" << VAR(target.directory().path() / temporary_name_) << VAR(status);
            return;
        }
        if (!handle_is_in_directory(handle_, target.directory().handle())) {
            LogError << "Temporary configuration left the trusted directory" << VAR(temporary_name_);
            return;
        }

        if (existing.exists()) {
            const auto result = SetSecurityInfo(
                handle_,
                SE_FILE_OBJECT,
                existing.security_information(),
                existing.owner(),
                existing.group(),
                existing.dacl(),
                existing.sacl());
            if (result != ERROR_SUCCESS) {
                LogError << "Failed to preserve configuration security information" << VAR(temporary_name_) << VAR(result);
                return;
            }
        }
#else
        descriptor_ = ::openat(target.directory().descriptor(), temporary_name_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor_ < 0) {
            LogError << "Failed to create temporary configuration" << VAR(temporary_name_) << VAR(errno);
            return;
        }

        if (existing.exists()) {
#if defined(__APPLE__) || defined(__linux__)
            existing_acl_ = existing.acl();
#endif
            target_mode_ = existing.status().st_mode & 07777;
            target_owner_ = existing.status().st_uid;
            target_group_ = existing.status().st_gid;
            const bool ownership_differs = target_owner_ != ::geteuid() || target_group_ != ::getegid();
            if (ownership_differs && ::fchown(descriptor_, target_owner_, target_group_) != 0) {
                LogError << "Failed to preserve configuration ownership" << VAR(temporary_name_) << VAR(errno);
                return;
            }
        }
#endif
        valid_ = true;
    }

    TemporaryConfigFile(const TemporaryConfigFile&) = delete;
    TemporaryConfigFile& operator=(const TemporaryConfigFile&) = delete;

    ~TemporaryConfigFile()
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor_ >= 0) {
            ::close(descriptor_);
            descriptor_ = -1;
        }
#endif
        if (!consumed_) {
            remove_from_trusted_directory(directory_descriptor_, temporary_name_);
        }
    }

    bool valid() const { return valid_; }

    bool write(std::string_view content)
    {
        size_t offset = 0;
        while (offset < content.size()) {
#ifdef _WIN32
            const auto chunk_size = static_cast<DWORD>(std::min<size_t>(content.size() - offset, 64 * 1024));
            DWORD written = 0;
            if (!WriteFile(handle_, content.data() + offset, chunk_size, &written, nullptr) || written == 0) {
                LogError << "Failed to write temporary configuration" << VAR(temporary_name_) << VAR(GetLastError());
                return false;
            }
#else
            const auto written = ::write(descriptor_, content.data() + offset, content.size() - offset);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LogError << "Failed to write temporary configuration" << VAR(temporary_name_) << VAR(errno);
                return false;
            }
            if (written == 0) {
                LogError << "Failed to write temporary configuration" << VAR(temporary_name_);
                return false;
            }
#endif
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    bool finish()
    {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) {
            LogError << "Failed to flush temporary configuration" << VAR(temporary_name_) << VAR(GetLastError());
            return false;
        }
#else
        if (::fchmod(descriptor_, target_mode_) != 0) {
            LogError << "Failed to preserve configuration permissions" << VAR(temporary_name_) << VAR(errno);
            return false;
        }
#if defined(__APPLE__)
        if (existing_acl_ != nullptr) {
            if (::acl_set_fd_np(descriptor_, existing_acl_, kConfigAclType) != 0) {
                LogError << "Failed to preserve configuration ACL" << VAR(temporary_name_) << VAR(errno);
                return false;
            }
        }
        else {
            acl_t empty_acl = ::acl_init(0);
            if (empty_acl == nullptr) {
                LogError << "Failed to create an empty configuration ACL" << VAR(temporary_name_) << VAR(errno);
                return false;
            }
            const bool cleared = ::acl_set_fd_np(descriptor_, empty_acl, kConfigAclType) == 0 || missing_acl_error(errno);
            ::acl_free(empty_acl);
            if (!cleared) {
                LogError << "Failed to clear inherited configuration ACL" << VAR(temporary_name_) << VAR(errno);
                return false;
            }
        }
#elif defined(__linux__)
        if (!existing_acl_.empty()) {
            if (::fsetxattr(descriptor_, kPosixAclXattr, existing_acl_.data(), existing_acl_.size(), 0) != 0) {
                LogError << "Failed to preserve configuration ACL" << VAR(temporary_name_) << VAR(errno);
                return false;
            }
        }
        else if (::fremovexattr(descriptor_, kPosixAclXattr) != 0 && !missing_xattr_error(errno)) {
            LogError << "Failed to clear inherited configuration ACL" << VAR(temporary_name_) << VAR(errno);
            return false;
        }
#endif
        if (::fsync(descriptor_) != 0) {
            LogError << "Failed to flush temporary configuration" << VAR(temporary_name_) << VAR(errno);
            return false;
        }
        if (::close(descriptor_) != 0) {
            descriptor_ = -1;
            LogError << "Failed to close temporary configuration" << VAR(temporary_name_) << VAR(errno);
            return false;
        }
        descriptor_ = -1;
#endif
        return true;
    }

    bool replace(const TrustedConfigurationTarget& target)
    {
#ifdef _WIN32
        const auto target_name = target_name_.wstring();
        const size_t byte_count = sizeof(FileRenameRequest) + target_name.size() * sizeof(wchar_t);
        std::vector<unsigned char> buffer(byte_count, 0);
        auto* information = reinterpret_cast<FileRenameRequest*>(buffer.data());
        information->flags = kFileRenameFlagReplaceIfExists | kFileRenameFlagPosixSemantics;
        information->root_directory = target.directory().handle();
        information->file_name_length = static_cast<DWORD>(target_name.size() * sizeof(wchar_t));
        std::memcpy(information->file_name, target_name.data(), information->file_name_length);
        NtIoStatusBlock io_status { };
        const NTSTATUS status =
            NtSetInformationFile(handle_, &io_status, information, static_cast<ULONG>(buffer.size()), kNtFileRenameInformationEx);
        if (!nt_success(status)) {
            LogError << "Failed to replace configuration" << VAR(temporary_name_) << VAR(target_name_) << VAR(status);
            return false;
        }
#else
        if (::renameat(target.directory().descriptor(), temporary_name_.c_str(), target.directory().descriptor(), target_name_.c_str())
            != 0) {
            LogError << "Failed to replace configuration" << VAR(temporary_name_) << VAR(target_name_) << VAR(errno);
            return false;
        }
#endif
        consumed_ = true;
        return true;
    }

private:
    static void remove_from_trusted_directory(
#ifdef _WIN32
        HANDLE directory,
#else
        int directory,
#endif
        const std::filesystem::path& name)
    {
#ifdef _WIN32
        const auto native_name = name.wstring();
        NtUnicodeString object_name = nt_name(native_name);
        NtObjectAttributes attributes = nt_relative_attributes(directory, object_name);
        NtDeleteFile(&attributes);
#else
        const auto native_name = std::filesystem::path(name).native();
        ::unlinkat(directory, native_name.c_str(), 0);
#endif
    }

    std::filesystem::path target_name_;
    std::filesystem::path temporary_name_;
    bool valid_ = false;
    bool consumed_ = false;
#ifdef _WIN32
    HANDLE directory_descriptor_ = INVALID_HANDLE_VALUE;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int directory_descriptor_ = -1;
    int descriptor_ = -1;
    mode_t target_mode_ = 0600;
    uid_t target_owner_ = 0;
    gid_t target_group_ = 0;
#if defined(__APPLE__)
    acl_t existing_acl_ = nullptr;
#elif defined(__linux__)
    std::string existing_acl_;
#endif
#endif
};

bool write_configuration(const Configuration& config, const TrustedConfigurationTarget& target, const ExistingConfigurationFile& existing)
{
    std::ostringstream stream;
    stream << config.to_json();
    if (!stream.good()) {
        LogError << "Failed to serialize configuration" << VAR(target.file_name());
        return false;
    }
    const auto serialized = stream.str();

    auto serialized_json = json::parse(serialized);
    if (!serialized_json || !Parser::parse_config(*serialized_json)) {
        LogError << "Failed to validate configuration" << VAR(target.file_name());
        return false;
    }

    TemporaryConfigFile temporary_file(target, existing);
    if (!temporary_file.valid() || !temporary_file.write(serialized) || !temporary_file.finish()) {
        return false;
    }
    return temporary_file.replace(target);
}

void merge_local_changes(const Configuration& baseline, const Configuration& local, Configuration& persisted)
{
    auto merge_field = [&]<typename Field>(Field Configuration::* field) {
        if (local.*field != baseline.*field) {
            persisted.*field = local.*field;
        }
    };

    merge_field(&Configuration::controller);
    merge_field(&Configuration::adb);
    merge_field(&Configuration::win32);
    merge_field(&Configuration::macos);
    merge_field(&Configuration::playcover);
    merge_field(&Configuration::gamepad);
    merge_field(&Configuration::lnx);
    merge_field(&Configuration::resource);
    merge_field(&Configuration::task);
    merge_field(&Configuration::global_option);
    merge_field(&Configuration::resource_option);
    merge_field(&Configuration::controller_option);
    merge_field(&Configuration::pretask);
    merge_field(&Configuration::enabled_pretask);

    // Welcome snapshots have their own locked update path and are not ordinary
    // configuration edits.
}

} // namespace

bool Configurator::load(const std::filesystem::path& resource_dir, const std::filesystem::path& user_dir)
{
    LogFunc << VAR(resource_dir) << VAR(user_dir);

    auto data_opt = Parser::parse_interface(resource_dir / kInterfaceFilename);
    if (!data_opt) {
        LogError << "Failed to parse interface.json";
        return false;
    }
    data_ = *std::move(data_opt);
    if (data_.resource.empty()) {
        LogError << "Resource is empty";
        return false;
    }

    if (auto cfg_opt = Parser::parse_config(user_dir / kConfigPath)) {
        config_ = *std::move(cfg_opt);
        first_time_use_ = false;
        if (!transform_stored_passwords(data_.option, config_, SecretStore::decrypt)) {
            LogError << "Failed to decrypt stored password inputs";
            return false;
        }
    }
    else {
        first_time_use_ = true;
    }

    resource_dir_ = resource_dir;
    loaded_config_ = config_;

    // 加载翻译文件
    load_translations();

    return true;
}

bool Configurator::check_configuration()
{
    LogFunc;

    if (first_time_use_) {
        return true;
    }

    return Parser::check_configuration(data_, config_);
}

bool Configurator::save(const std::filesystem::path& user_dir)
{
    LogInfo << VAR(user_dir);

    const auto config_path = user_dir / kConfigPath;
    const auto target_path = resolve_configuration_target(config_path);
    if (!target_path) {
        return false;
    }

    ConfigFileLock lock(*target_path);
    if (!lock.valid()) {
        return false;
    }

    ExistingConfigurationFile existing_file(*target_path);
    if (!existing_file.valid()) {
        return false;
    }

    auto persisted_config = config_;
    if (existing_file.exists()) {
        auto latest_config = existing_file.parse_config();
        if (!latest_config) {
            LogError << "Failed to reload configuration" << VAR(target_path->directory().path() / target_path->file_name());
            return false;
        }
        persisted_config = *std::move(latest_config);
        if (!transform_stored_passwords(data_.option, persisted_config, SecretStore::decrypt)) {
            LogError << "Failed to decrypt persisted password inputs";
            return false;
        }
    }

    merge_local_changes(loaded_config_, config_, persisted_config);

    auto stored_config = persisted_config;
    if (!transform_stored_passwords(data_.option, stored_config, SecretStore::encrypt)) {
        LogError << "Refusing to save configuration with an encryption failure";
        return false;
    }

    if (!write_configuration(stored_config, *target_path, existing_file)) {
        return false;
    }

    config_ = std::move(persisted_config);
    loaded_config_ = config_;
    return true;
}

std::optional<bool> Configurator::update_welcome_snapshots(
    const std::filesystem::path& user_dir,
    std::vector<std::string> declared,
    std::vector<std::string> resolved)
{
    const auto config_path = user_dir / kConfigPath;
    const auto target_path = resolve_configuration_target(config_path);
    if (!target_path) {
        return std::nullopt;
    }

    ConfigFileLock lock(*target_path);
    if (!lock.valid()) {
        return std::nullopt;
    }

    ExistingConfigurationFile existing_file(*target_path);
    if (!existing_file.valid()) {
        return std::nullopt;
    }
    if (!existing_file.exists()) {
        LogError << "Cannot update welcome snapshots before a configuration exists"
                 << VAR(target_path->directory().path() / target_path->file_name());
        return std::nullopt;
    }

    auto persisted_config = existing_file.parse_config();
    if (!persisted_config) {
        LogError << "Failed to reload configuration for welcome snapshots"
                 << VAR(target_path->directory().path() / target_path->file_name());
        return std::nullopt;
    }

    auto latest_config = *std::move(persisted_config);
    if (!transform_stored_passwords(data_.option, latest_config, SecretStore::decrypt)) {
        LogError << "Failed to decrypt persisted password inputs for welcome snapshots";
        return std::nullopt;
    }

    if (latest_config.last_welcome == declared && latest_config.last_resolved_welcome == resolved) {
        auto& config = configuration();
        config.last_welcome = latest_config.last_welcome;
        config.last_resolved_welcome = latest_config.last_resolved_welcome;
        loaded_config_.last_welcome = config.last_welcome;
        loaded_config_.last_resolved_welcome = config.last_resolved_welcome;
        return false;
    }

    latest_config.last_welcome = std::move(declared);
    latest_config.last_resolved_welcome = std::move(resolved);

    auto stored_config = latest_config;
    if (!transform_stored_passwords(data_.option, stored_config, SecretStore::encrypt)) {
        LogError << "Refusing to save welcome snapshots with an encryption failure";
        return std::nullopt;
    }

    if (!write_configuration(stored_config, *target_path, existing_file)) {
        return std::nullopt;
    }

    auto& config = configuration();
    config.last_welcome = latest_config.last_welcome;
    config.last_resolved_welcome = latest_config.last_resolved_welcome;
    loaded_config_.last_welcome = config.last_welcome;
    loaded_config_.last_resolved_welcome = config.last_resolved_welcome;
    return true;
}

std::optional<RuntimeParam> Configurator::generate_runtime() const
{
    RuntimeParam runtime;

    auto resource_iter = std::ranges::find_if(data_.resource, [&](const auto& resource) { return resource.name == config_.resource; });

    if (resource_iter == data_.resource.end()) {
        LogWarn << "Resource not found";
        return std::nullopt;
    }

    for (const auto& path_string : resource_iter->path) {
        // v2: relative path from interface.json directory
        runtime.resource_path.emplace_back(resource_dir_ / MaaNS::path(path_string));
    }
    if (runtime.resource_path.empty()) {
        LogWarn << "No resource to load";
        return std::nullopt;
    }
    runtime.primary_resource_count = runtime.resource_path.size();
    runtime.resource_hash = resource_iter->hash;

    // Find current controller for attach_resource_path

    auto controller_iter =
        std::ranges::find_if(data_.controller, [&](const auto& controller) { return controller.name == config_.controller.name; });
    if (controller_iter == data_.controller.end()) {
        LogWarn << "Controller not found" << VAR(config_.controller.name);
        return std::nullopt;
    }
    auto& controller = *controller_iter;

    // Append attach_resource_path after resource.path
    for (const auto& attach_path : controller.attach_resource_path) {
        runtime.resource_path.emplace_back(resource_dir_ / MaaNS::path(attach_path));
    }

    for (const auto& config_task : config_.task) {
        auto data_task_iter = std::ranges::find_if(data_.task, [&](const auto& data_task) { return data_task.name == config_task.name; });
        if (data_task_iter != data_.task.end() && !is_task_applicable(*data_task_iter)) {
            continue;
        }

        auto task_opt = generate_runtime_task(config_task);
        if (!task_opt) {
            LogWarn << "failed to generate runtime, ignore" << VAR(config_task.name);
            continue;
        }
        runtime.task.emplace_back(*std::move(task_opt));
    }
    if (runtime.task.empty()) {
        LogWarn << "No task to run";
        return std::nullopt;
    }

    switch (controller.type) {
    case InterfaceData::Controller::Type::Adb: {
        RuntimeParam::AdbParam adb;

        adb.name = config_.adb.name;
        adb.adb_path = config_.adb.adb_path;
        adb.address = config_.adb.address;
        adb.agent_path = MaaNS::path_to_utf8_string(resource_dir_ / "MaaAgentBinary");

        runtime.controller_param = std::move(adb);
    } break;

    case InterfaceData::Controller::Type::Win32: {
        RuntimeParam::Win32Param win32;

        win32.hwnd = config_.win32.hwnd;

        // v2: parse from config, use default if not specified or invalid
        if (!controller.win32.screencap.empty()) {
            win32.screencap = parse_win32_screencap_method(controller.win32.screencap);
        }
        if (win32.screencap == MaaWin32ScreencapMethod_None) {
            win32.screencap = MaaWin32ScreencapMethod_DXGI_DesktopDup;
        }

        if (!controller.win32.mouse.empty()) {
            win32.mouse = parse_win32_input_method(controller.win32.mouse);
        }
        if (win32.mouse == MaaWin32InputMethod_None) {
            win32.mouse = MaaWin32InputMethod_Seize;
        }

        if (!controller.win32.keyboard.empty()) {
            win32.keyboard = parse_win32_input_method(controller.win32.keyboard);
        }
        if (win32.keyboard == MaaWin32InputMethod_None) {
            win32.keyboard = MaaWin32InputMethod_Seize;
        }

        runtime.controller_param = std::move(win32);
    } break;

    case InterfaceData::Controller::Type::MacOS: {
        RuntimeParam::MacOSParam macos;

        macos.window_id = config_.macos.window_id;

        // v2: parse from config, use default if not specified or invalid
        if (!controller.macos.screencap.empty()) {
            macos.screencap = parse_macos_screencap_method(controller.macos.screencap);
        }
        if (macos.screencap == MaaMacOSScreencapMethod_None) {
            macos.screencap = MaaMacOSScreencapMethod_ScreenCaptureKit;
        }

        if (!controller.macos.input.empty()) {
            macos.input = parse_macos_input_method(controller.macos.input);
        }
        if (macos.input == MaaMacOSInputMethod_None) {
            macos.input = MaaMacOSInputMethod_GlobalEvent;
        }

        runtime.controller_param = std::move(macos);
    } break;

    case InterfaceData::Controller::Type::PlayCover: {
        RuntimeParam::PlayCoverParam playcover;

        playcover.address = config_.playcover.address;
        playcover.uuid = config_.playcover.uuid.empty() ? "maa.playcover" : config_.playcover.uuid;

        if (playcover.address.empty()) {
            LogError << "PlayCover address is empty";
            return std::nullopt;
        }

        runtime.controller_param = std::move(playcover);
    } break;

    case InterfaceData::Controller::Type::Gamepad: {
        RuntimeParam::GamepadParam gamepad;

        gamepad.hwnd = config_.gamepad.hwnd;
        gamepad.gamepad_type =
            parse_gamepad_type(config_.gamepad.gamepad_type.empty() ? controller.gamepad.gamepad_type : config_.gamepad.gamepad_type);

        if (!controller.gamepad.screencap.empty()) {
            gamepad.screencap = parse_win32_screencap_method(controller.gamepad.screencap);
        }
        if (gamepad.screencap == MaaWin32ScreencapMethod_None && gamepad.hwnd != nullptr) {
            gamepad.screencap = MaaWin32ScreencapMethod_DXGI_DesktopDup;
        }

        runtime.controller_param = std::move(gamepad);
    } break;

    case InterfaceData::Controller::Type::Linux: {
        RuntimeParam::LinuxParam lnx;

        lnx.use_win32_vk_code = controller.lnx.use_win32_vk_code;
        lnx.pipewire_source = controller.lnx.pipewire_source.empty() ? "Gamescope" : controller.lnx.pipewire_source;

        if (!controller.lnx.screencap.empty()) {
            lnx.screencap = parse_linux_screencap_method(controller.lnx.screencap);
        }
        if (lnx.screencap == MaaLinuxScreencapMethod_None) {
            lnx.screencap = MaaLinuxScreencapMethod_Wlr;
        }

        if (!controller.lnx.input.empty()) {
            lnx.input = parse_linux_input_method(controller.lnx.input);
        }
        if (lnx.input == MaaLinuxInputMethod_None) {
            lnx.input = MaaLinuxInputMethod_Wlr;
        }

        lnx.wlr_socket_path = config_.lnx.wlr_socket_path;
        lnx.uinput_screen_width = config_.lnx.uinput_screen_width;
        lnx.uinput_screen_height = config_.lnx.uinput_screen_height;
        lnx.eis_socket_path = config_.lnx.eis_socket_path;

        runtime.controller_param = std::move(lnx);
    } break;

    default: {
        LogError << "Unknown controller type" << VAR(controller.type);
        return std::nullopt;
    }
    }

    // 设置分辨率配置
    runtime.display_config.short_side = controller.display_short_side;
    runtime.display_config.long_side = controller.display_long_side;
    runtime.display_config.expand = controller.display_expand;
    runtime.display_config.raw = controller.display_raw;

    for (const auto& pretask_config : Parser::flatten_pretask(data_.pretask)) {
        const auto pretask_name = pretask_identifier(pretask_config);
        if (config_.enabled_pretask && std::ranges::find(*config_.enabled_pretask, pretask_name) == config_.enabled_pretask->end()) {
            continue;
        }
        if (!pretask_config.resource.empty()
            && std::ranges::find(pretask_config.resource, config_.resource) == pretask_config.resource.end()) {
            continue;
        }
        if (!pretask_config.controller.empty()
            && std::ranges::find(pretask_config.controller, controller.name) == pretask_config.controller.end()) {
            continue;
        }

        RuntimeParam::Pretask runtime_pretask;
        runtime_pretask.name = pretask_name;
        runtime_pretask.exec = MaaNS::path(pretask_config.exec);
        runtime_pretask.args = pretask_config.args;
        runtime_pretask.cwd = resource_dir_;

        if (!pretask_config.option.empty()) {
            auto config_pretask_iter = std::ranges::find_if(config_.pretask, [&](const auto& config_pretask) {
                return config_pretask.name == runtime_pretask.name;
            });
            if (config_pretask_iter == config_.pretask.end()) {
                LogError << "Pretask config not found" << VAR(runtime_pretask.name);
                return std::nullopt;
            }

            json::object options;
            std::unordered_set<std::string> expanding;
            for (const auto& option_name : pretask_config.option) {
                if (!append_pretask_option(option_name, *config_pretask_iter, options, expanding)) {
                    LogError << "Failed to generate pretask option" << VAR(runtime_pretask.name) << VAR(option_name);
                    return std::nullopt;
                }
            }
            runtime_pretask.args.emplace_back(options.dumps());
        }

        runtime.pretask.emplace_back(std::move(runtime_pretask));
    }

    std::vector<InterfaceData::Agent> agents = std::visit(
        [](auto&& arg) -> std::vector<InterfaceData::Agent> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, InterfaceData::Agent>) {
                return { arg };
            }
            else {
                return arg;
            }
        },
        data_.agent);

    // v2.5.0: prepare PI_* env vars
    std::unordered_map<std::string, std::string> pi_env;
    pi_env["PI_INTERFACE_VERSION"] = "v2.6.0";
    pi_env["PI_CLIENT_NAME"] = "MaaPiCli";
    pi_env["PI_CLIENT_VERSION"] = MaaVersion();
    pi_env["PI_CLIENT_LANGUAGE"] = detect_system_language();
    pi_env["PI_CLIENT_MAAFW_VERSION"] = MaaVersion();
    if (!data_.version.empty()) {
        pi_env["PI_VERSION"] = data_.version;
    }

    // PI_CONTROLLER: serialize selected controller with i18n resolved
    {
        json::value ctrl_json = controller.to_json();
        if (ctrl_json.contains("label")) {
            ctrl_json["label"] = translate(controller.label);
        }
        if (ctrl_json.contains("description")) {
            ctrl_json["description"] = translate(controller.description);
        }
        pi_env["PI_CONTROLLER"] = ctrl_json.dumps();
    }

    // PI_RESOURCE: serialize selected resource with i18n resolved
    {
        json::value res_json = resource_iter->to_json();
        if (res_json.contains("label")) {
            res_json["label"] = translate(resource_iter->label);
        }
        if (res_json.contains("description")) {
            res_json["description"] = translate(resource_iter->description);
        }
        pi_env["PI_RESOURCE"] = res_json.dumps();
    }

    for (const auto& agent_config : agents) {
        if (agent_config.child_exec.empty()) {
            continue;
        }

        RuntimeParam::Agent agent;
        agent.child_exec = MaaNS::path(agent_config.child_exec);
        agent.child_args = agent_config.child_args;
        agent.identifier = agent_config.identifier;
        agent.cwd = resource_dir_;
        agent.env_vars = pi_env;

        runtime.agent.emplace_back(std::move(agent));
    }

    return runtime;
}

bool Configurator::is_option_applicable(const InterfaceData::Option& opt) const
{
    if (!opt.controller.empty() && std::ranges::find(opt.controller, config_.controller.name) == opt.controller.end()) {
        return false;
    }
    if (!opt.resource.empty() && std::ranges::find(opt.resource, config_.resource) == opt.resource.end()) {
        return false;
    }
    return true;
}

bool Configurator::is_task_applicable(const InterfaceData::Task& task) const
{
    if (!task.controller.empty() && std::ranges::find(task.controller, config_.controller.name) == task.controller.end()) {
        return false;
    }
    if (!task.resource.empty() && std::ranges::find(task.resource, config_.resource) == task.resource.end()) {
        return false;
    }
    return true;
}

void Configurator::merge_option_overrides(RuntimeParam::Task& runtime_task, const std::vector<Configuration::Option>& config_options) const
{
    for (const auto& config_option : config_options) {
        auto data_option_iter = data_.option.find(config_option.name);
        if (data_option_iter == data_.option.end()) {
            LogWarn << "option not found" << VAR(config_option.name);
            continue;
        }
        const auto& data_option = data_option_iter->second;

        if (!is_option_applicable(data_option)) {
            continue;
        }

        switch (data_option.type) {
        case InterfaceData::Option::Type::Select:
        case InterfaceData::Option::Type::Switch: {
            auto data_case_iter =
                std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == config_option.value; });
            if (data_case_iter == data_option.cases.end()) {
                LogWarn << "case not found" << VAR(config_option.value);
                continue;
            }
            runtime_task.pipeline_override.emplace(data_case_iter->pipeline_override);
        } break;

        case InterfaceData::Option::Type::Checkbox: {
            for (const auto& data_case : data_option.cases) {
                if (std::ranges::find(config_option.values, data_case.name) != config_option.values.end()) {
                    runtime_task.pipeline_override.emplace(data_case.pipeline_override);
                }
            }
        } break;

        case InterfaceData::Option::Type::Input: {
            std::string override_str = data_option.pipeline_override.dumps();
            for (const auto& input_def : data_option.inputs) {
                std::string placeholder = "{" + input_def.name + "}";
                std::string value;
                if (auto it = config_option.inputs.find(input_def.name); it != config_option.inputs.end()) {
                    value = it->second;
                }
                else {
                    value = input_def.default_;
                }

                switch (input_def.pipeline_type) {
                case InterfaceData::Option::Input::PipelineType::String:
                    override_str = MaaNS::string_replace_all(override_str, "\"" + placeholder + "\"", "\"" + value + "\"");
                    override_str = MaaNS::string_replace_all(override_str, placeholder, value);
                    break;
                case InterfaceData::Option::Input::PipelineType::Int:
                case InterfaceData::Option::Input::PipelineType::Bool:
                    override_str = MaaNS::string_replace_all(override_str, "\"" + placeholder + "\"", value);
                    override_str = MaaNS::string_replace_all(override_str, placeholder, value);
                    break;
                }
            }
            if (auto parsed = json::parse(override_str)) {
                runtime_task.pipeline_override.emplace(parsed->as_object());
            }
            else {
                LogWarn << "Failed to parse pipeline override JSON for input option" << VAR(config_option.name);
            }
        } break;
        }
    }
}

std::optional<RuntimeParam::Task> Configurator::generate_runtime_task(const Configuration::Task& config_task) const
{
    auto data_iter = std::ranges::find_if(data_.task, [&](const auto& data_task) { return data_task.name == config_task.name; });
    if (data_iter == data_.task.end()) {
        LogWarn << "task not found" << VAR(config_task.name);
        return std::nullopt;
    }
    const auto& data_task = *data_iter;

    RuntimeParam::Task runtime_task { .name = data_task.name,
                                      .entry = data_task.entry,
                                      .pipeline_override = json::array { data_task.pipeline_override } };

    // v2.3.0: merge in priority order (later overrides earlier)
    // 1. global_option (lowest priority)
    merge_option_overrides(runtime_task, config_.global_option);
    // 2. resource.option
    merge_option_overrides(runtime_task, config_.resource_option);
    // 3. controller.option
    merge_option_overrides(runtime_task, config_.controller_option);
    // 4. task.option (highest priority)
    merge_option_overrides(runtime_task, config_task.option);

    return runtime_task;
}

bool Configurator::append_pretask_option(
    const std::string& option_name,
    const Configuration::Pretask& config_pretask,
    json::object& options,
    std::unordered_set<std::string>& expanding) const
{
    if (!expanding.emplace(option_name).second) {
        LogError << "Recursive pretask option reference" << VAR(option_name);
        return false;
    }
    OnScopeLeave([&]() { expanding.erase(option_name); });

    auto data_option_iter = data_.option.find(option_name);
    if (data_option_iter == data_.option.end()) {
        LogError << "Pretask option not found" << VAR(option_name);
        return false;
    }
    const auto& data_option = data_option_iter->second;
    if (!is_option_applicable(data_option)) {
        return true;
    }

    auto config_option_iter = std::ranges::find_if(config_pretask.option, [&](const auto& option) { return option.name == option_name; });
    if (config_option_iter == config_pretask.option.end()) {
        LogError << "Pretask option config not found" << VAR(option_name);
        return false;
    }
    const auto& config_option = *config_option_iter;

    switch (data_option.type) {
    case InterfaceData::Option::Type::Select:
    case InterfaceData::Option::Type::Switch: {
        auto data_case_iter =
            std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == config_option.value; });
        if (data_case_iter == data_option.cases.end()) {
            LogError << "Pretask option case not found" << VAR(option_name) << VAR(config_option.value);
            return false;
        }

        options[option_name] = config_option.value;
        for (const auto& sub_option_name : data_case_iter->option) {
            if (!append_pretask_option(sub_option_name, config_pretask, options, expanding)) {
                return false;
            }
        }
    } break;

    case InterfaceData::Option::Type::Checkbox: {
        json::array checked_values;
        for (const auto& value : config_option.values) {
            auto data_case_iter = std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == value; });
            if (data_case_iter == data_option.cases.end()) {
                LogError << "Pretask option case not found" << VAR(option_name) << VAR(value);
                return false;
            }
            checked_values.emplace_back(value);
        }
        options[option_name] = std::move(checked_values);

        for (const auto& value : config_option.values) {
            auto data_case_iter = std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == value; });
            if (data_case_iter == data_option.cases.end()) {
                return false;
            }
            for (const auto& sub_option_name : data_case_iter->option) {
                if (!append_pretask_option(sub_option_name, config_pretask, options, expanding)) {
                    return false;
                }
            }
        }
    } break;

    case InterfaceData::Option::Type::Input: {
        json::object input_values;
        for (const auto& input_def : data_option.inputs) {
            std::string value = input_def.default_;
            if (auto value_iter = config_option.inputs.find(input_def.name); value_iter != config_option.inputs.end()) {
                value = value_iter->second;
            }
            input_values[input_def.name] = std::move(value);
        }
        options[option_name] = std::move(input_values);
    } break;
    }

    return true;
}

std::string Configurator::detect_system_language() const
{
    // 尝试获取系统语言
    std::string lang;

#ifdef _WIN32
    // Windows: 使用 GetUserDefaultLocaleName
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
        lang = MaaNS::from_u16(locale_name);
    }
#else
    // Unix-like: 使用环境变量
    if (const char* lc_all = std::getenv("LC_ALL"); lc_all && *lc_all) {
        lang = lc_all;
    }
    else if (const char* lc_messages = std::getenv("LC_MESSAGES"); lc_messages && *lc_messages) {
        lang = lc_messages;
    }
    else if (const char* language = std::getenv("LANG"); language && *language) {
        lang = language;
    }
#endif

    // 转换为小写并替换 - 为 _
    std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char c) { return std::tolower(c); });
    std::replace(lang.begin(), lang.end(), '-', '_');

    // 提取语言代码 (例如 zh_cn.utf-8 -> zh_cn)
    if (auto pos = lang.find('.'); pos != std::string::npos) {
        lang = lang.substr(0, pos);
    }

    return lang;
}

void Configurator::load_translations()
{
    if (data_.languages.empty()) {
        return;
    }

    std::string sys_lang = detect_system_language();
    LogInfo << "System language:" << sys_lang;

    // 查找匹配的语言文件
    std::string translation_file;

    // 1. 精确匹配 (例如 zh_cn)
    if (auto it = data_.languages.find(sys_lang); it != data_.languages.end()) {
        translation_file = it->second;
    }
    // 2. 语言前缀匹配 (例如 zh_cn 匹配 zh)
    else if (auto pos = sys_lang.find('_'); pos != std::string::npos) {
        std::string lang_prefix = sys_lang.substr(0, pos);
        if (auto prefix_it = data_.languages.find(lang_prefix); prefix_it != data_.languages.end()) {
            translation_file = prefix_it->second;
        }
    }
    // 3. 尝试匹配任何以系统语言开头的语言（仅当 sys_lang 非空时）
    // 注意：starts_with("") 会匹配任何字符串，所以必须检查 sys_lang 是否为空
    if (translation_file.empty() && sys_lang.size() >= 2) {
        std::string lang_prefix = sys_lang.substr(0, 2);
        for (const auto& [lang_code, file] : data_.languages) {
            if (lang_code.starts_with(lang_prefix)) {
                translation_file = file;
                break;
            }
        }
    }

    if (translation_file.empty()) {
        LogInfo << "No matching translation file found for language:" << sys_lang;
        return;
    }

    // 加载翻译文件
    auto translation_path = resource_dir_ / translation_file;
    LogInfo << "Loading translation file:" << translation_path;

    std::ifstream ifs(translation_path);
    if (!ifs.is_open()) {
        LogWarn << "Failed to open translation file:" << translation_path;
        return;
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();
    auto json_opt = json::parse(buffer.str());
    if (!json_opt || !json_opt->is_object()) {
        LogWarn << "Failed to parse translation file:" << translation_path;
        return;
    }

    for (const auto& [key, value] : json_opt->as_object()) {
        if (value.is_string()) {
            translations_[key] = value.as_string();
        }
    }

    LogInfo << "Loaded" << translations_.size() << "translations";
}

std::string Configurator::translate(const std::string& text) const
{
    if (text.empty()) {
        return text;
    }

    // 如果文本以 $ 开头，则从翻译表中查找
    if (text[0] == '$') {
        std::string key = text.substr(1); // 去掉 $ 前缀
        if (auto it = translations_.find(key); it != translations_.end()) {
            return it->second;
        }
        // 如果没找到翻译，返回去掉 $ 的原文
        return key;
    }

    return text;
}

MAA_PROJECT_INTERFACE_NS_END
