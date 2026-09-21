#include "ProjectInterface/Runner.h"

#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>

#ifdef _WIN32
#include <system_error>
#endif

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <meojson/json.hpp>

#include "MaaAgentClient/MaaAgentClientAPI.h"
#include "MaaFramework/MaaAPI.h"
#include "MaaToolkit/MaaToolkitAPI.h"

#include "Common/MaaTypes.h"
#include "MaaUtils/Encoding.h"
#include "MaaUtils/Logger.h"
#include "MaaUtils/Platform.h"
#include "MaaUtils/ScopeLeave.hpp"
#include "MaaUtils/StringMisc.hpp"

MAA_PROJECT_INTERFACE_NS_BEGIN

namespace
{

#ifdef _WIN32
std::wstring quote_argument(const std::wstring& arg)
{
    if (arg.empty()) {
        return L"\"\"";
    }

    if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        }
        else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(ch);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::vector<std::wstring> conv_args(const std::vector<std::string>& args)
{
    std::vector<std::wstring> wargs;
    for (const auto& arg : args) {
        wargs.emplace_back(to_u16(arg));
    }
    return wargs;
}

std::string createprocess_error_hint(DWORD error_code)
{
    switch (error_code) {
    case ERROR_FILE_NOT_FOUND:
        return "Executable not found. Check PATH or use an absolute path for child_exec (e.g. the full path to python.exe).";
    case ERROR_PATH_NOT_FOUND:
        return "Path not found. Check child_exec and cwd paths. Paths in child_args are validated after the child starts.";
    case ERROR_DIRECTORY:
        return "The working directory (cwd) is invalid or does not exist.";
    case ERROR_ACCESS_DENIED:
        return "Access denied. MaaPiCli may need to run as administrator.";
    case ERROR_BAD_EXE_FORMAT:
        return "Bad executable format. Ensure child_exec points to a runnable Windows executable (e.g. .exe); required DLL dependencies "
               "must also be available.";
    case ERROR_ELEVATION_REQUIRED:
        return "Elevation required. Run MaaPiCli as administrator, or use a child_exec that does not need elevation.";
    case ERROR_DLL_NOT_FOUND:
        return "A required DLL was not found. The child executable may need its dependencies on PATH.";
    case ERROR_MOD_NOT_FOUND:
        return "A required module was not found. Check the environment/dependencies of the child process.";
    default:
        return "Failed to launch child process. Verify child_exec is on PATH and cwd is valid. Paths in child_args are validated after the "
               "child starts.";
    }
}

bool run_pretask_process(const RuntimeParam::Pretask& pretask)
{
    std::wstring command_line = quote_argument(pretask.exec.native());
    for (const auto& arg : conv_args(pretask.args)) {
        command_line.push_back(L' ');
        command_line += quote_argument(arg);
    }

    STARTUPINFOW startup_info = { .cb = sizeof(startup_info) };
    PROCESS_INFORMATION process_info = { };
    if (!CreateProcessW(
            nullptr,
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            pretask.cwd.native().c_str(),
            &startup_info,
            &process_info)) {
        DWORD error_code = ::GetLastError();
        std::string error_message = std::system_category().message(static_cast<int>(error_code));
        std::string error_hint = createprocess_error_hint(error_code);
        LogError << "Failed to CreateProcessW" << VAR(pretask.exec) << VAR(pretask.cwd) << VAR(error_code) << VAR(error_message)
                 << VAR(error_hint);
        return false;
    }

    CloseHandle(process_info.hThread);
    WaitForSingleObject(process_info.hProcess, INFINITE);

    DWORD exit_code = 1;
    bool success = GetExitCodeProcess(process_info.hProcess, &exit_code) && exit_code == 0;
    CloseHandle(process_info.hProcess);
    return success;
}

class AgentProcess
{
public:
    explicit AgentProcess(HANDLE process)
        : process_(process)
    {
    }

    AgentProcess(AgentProcess&& other) noexcept
        : process_(std::exchange(other.process_, INVALID_HANDLE_VALUE))
    {
    }

    AgentProcess& operator=(AgentProcess&& other) noexcept
    {
        if (this != &other) {
            close();
            process_ = std::exchange(other.process_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    ~AgentProcess() { close(); }

    bool valid() const { return process_ != INVALID_HANDLE_VALUE && process_ != nullptr; }

private:
    void close()
    {
        if (!valid()) {
            return;
        }

        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, INFINITE);
        CloseHandle(process_);
        process_ = INVALID_HANDLE_VALUE;
    }

    HANDLE process_ = INVALID_HANDLE_VALUE;
};

std::unique_ptr<AgentProcess>
    spawn_agent(const std::filesystem::path& executable, const std::vector<std::wstring>& args, const std::filesystem::path& cwd)
{
    std::wstring command_line = quote_argument(executable.native());
    for (const auto& arg : args) {
        command_line.push_back(L' ');
        command_line += quote_argument(arg);
    }

    STARTUPINFOW startup_info = { .cb = sizeof(startup_info) };
    PROCESS_INFORMATION process_info = { };
    if (!CreateProcessW(
            nullptr,
            command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            cwd.native().c_str(),
            &startup_info,
            &process_info)) {
        DWORD error_code = ::GetLastError();
        std::string error_message = std::system_category().message(static_cast<int>(error_code));
        std::string error_hint = createprocess_error_hint(error_code);
        LogError << "Failed to CreateProcessW" << VAR(executable) << VAR(cwd) << VAR(error_code) << VAR(error_message) << VAR(error_hint);
        return nullptr;
    }

    CloseHandle(process_info.hThread);
    return std::make_unique<AgentProcess>(process_info.hProcess);
}
#else
class AgentProcess
{
public:
    explicit AgentProcess(pid_t pid)
        : pid_(pid)
    {
    }

    AgentProcess(AgentProcess&& other) noexcept
        : pid_(std::exchange(other.pid_, 0))
    {
    }

    AgentProcess& operator=(AgentProcess&& other) noexcept
    {
        if (this != &other) {
            close();
            pid_ = std::exchange(other.pid_, 0);
        }
        return *this;
    }

    ~AgentProcess() { close(); }

    bool valid() const { return pid_ > 0; }

private:
    void close()
    {
        if (!valid()) {
            return;
        }

        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
        pid_ = 0;
    }

    pid_t pid_ = 0;
};

std::unique_ptr<AgentProcess>
    spawn_agent(const std::filesystem::path& executable, const std::vector<std::string>& args, const std::filesystem::path& cwd)
{
    std::vector<char*> argv;
    argv.emplace_back(const_cast<char*>(executable.native().c_str()));
    for (const auto& arg : args) {
        argv.emplace_back(const_cast<char*>(arg.c_str()));
    }
    argv.emplace_back(nullptr);

    int exec_failed[2];
    if (pipe(exec_failed) != 0) {
        return nullptr;
    }
    if (fcntl(exec_failed[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(exec_failed[0]);
        close(exec_failed[1]);
        return nullptr;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(exec_failed[0]);
        if (chdir(cwd.native().c_str()) != 0) {
            _exit(127);
        }
        execvp(executable.native().c_str(), argv.data());
        char failed = 1;
        std::ignore = write(exec_failed[1], &failed, sizeof(failed));
        _exit(127);
    }

    close(exec_failed[1]);
    char failed = 0;
    ssize_t size = read(exec_failed[0], &failed, sizeof(failed));
    close(exec_failed[0]);

    if (pid <= 0 || (size == sizeof(failed) && failed != 0)) {
        if (pid > 0) {
            waitpid(pid, nullptr, 0);
        }
        return nullptr;
    }

    return std::make_unique<AgentProcess>(pid);
}

std::vector<std::string> conv_args(const std::vector<std::string>& args)
{
    return args;
}

bool run_pretask_process(const RuntimeParam::Pretask& pretask)
{
    std::vector<char*> argv;
    argv.emplace_back(const_cast<char*>(pretask.exec.native().c_str()));
    for (const auto& arg : pretask.args) {
        argv.emplace_back(const_cast<char*>(arg.c_str()));
    }
    argv.emplace_back(nullptr);

    int exec_failed[2];
    if (pipe(exec_failed) != 0) {
        return false;
    }
    if (fcntl(exec_failed[1], F_SETFD, FD_CLOEXEC) == -1) {
        close(exec_failed[0]);
        close(exec_failed[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(exec_failed[0]);
        if (chdir(pretask.cwd.native().c_str()) != 0) {
            _exit(127);
        }
        execvp(pretask.exec.native().c_str(), argv.data());
        char failed = 1;
        std::ignore = write(exec_failed[1], &failed, sizeof(failed));
        _exit(127);
    }

    close(exec_failed[1]);
    char failed = 0;
    ssize_t size = read(exec_failed[0], &failed, sizeof(failed));
    close(exec_failed[0]);

    if (pid <= 0 || (size == sizeof(failed) && failed != 0)) {
        if (pid > 0) {
            int status = 0;
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
            }
        }
        return false;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

}

RuntimeParam::AdbParam reconfig_adb(const RuntimeParam::AdbParam& raw)
{
    auto list_handle = MaaToolkitAdbDeviceListCreate();
    OnScopeLeave([&]() { MaaToolkitAdbDeviceListDestroy(list_handle); });

    MaaToolkitAdbDeviceFind(list_handle);

    size_t size = MaaToolkitAdbDeviceListSize(list_handle);
    for (size_t i = 0; i < size; ++i) {
        auto device_handle = MaaToolkitAdbDeviceListAt(list_handle, i);

        std::string name = MaaToolkitAdbDeviceGetName(device_handle);
        std::string path = MaaToolkitAdbDeviceGetAdbPath(device_handle);

        if (name != raw.name || path != raw.adb_path) {
            continue;
        }

        LogInfo << "Reconfigure ADB Param" << VAR(name) << VAR(path);

        std::string new_address = MaaToolkitAdbDeviceGetAddress(device_handle);
        if (raw.address != new_address) {
            LogInfo << "ADB Address changed" << VAR(raw.address) << VAR(new_address);
        }

        RuntimeParam::AdbParam new_param = raw;
        new_param.address = new_address;
        new_param.input = MaaToolkitAdbDeviceGetInputMethods(device_handle);
        new_param.screencap = MaaToolkitAdbDeviceGetScreencapMethods(device_handle);
        new_param.config = MaaToolkitAdbDeviceGetConfig(device_handle);
        return new_param;
    }

    return raw;
}

std::string reconfig_linux(const RuntimeParam::LinuxParam& raw)
{
    json::object obj {
        { "screencap_method", raw.screencap },
        { "input_method", raw.input },
        { "use_win32_vk_code", raw.use_win32_vk_code },
    };

    if (raw.screencap == MaaLinuxScreencapMethod_Wlr || raw.input == MaaLinuxInputMethod_Wlr) {
        obj["wlr_socket_path"] = raw.wlr_socket_path;
    }

    if (raw.screencap == MaaLinuxScreencapMethod_PipeWire) {
        if (raw.pipewire_source == "Gamescope") {
            auto instance_list = MaaToolkitGamescopeInstanceListCreate();
            OnScopeLeave([&]() { MaaToolkitGamescopeInstanceListDestroy(instance_list); });

            if (!MaaToolkitGamescopeInstanceFindAll(instance_list)) {
                LogError << "Failed to find gamescope instances";
                return "";
            }

            uint32_t pw_node_id = 0;
            for (MaaSize i = 0; i < MaaToolkitGamescopeInstanceListSize(instance_list); ++i) {
                auto instance = MaaToolkitGamescopeInstanceListAt(instance_list, i);
                if (uint32_t node_id = MaaToolkitGamescopeInstanceGetPipeWireNodeId(instance); node_id != 0) {
                    pw_node_id = node_id;
                    break;
                }
            }

            if (pw_node_id == 0) {
                LogError << "No gamescope PipeWire node found";
                return "";
            }

            obj["pw_node_id"] = pw_node_id;
        }
        else {
            auto helper_handle = MaaToolkitPortalHelperCreate();
            if (!helper_handle) {
                LogError << "Failed to create portal helper";
                return "";
            }

            if (!MaaToolkitPortalHelperOpenStream(helper_handle)) {
                LogError << "Failed to open PipeWire stream";
                MaaToolkitPortalHelperDestroy(helper_handle);
                return "";
            }

            obj["pw_socket_fd"] = MaaToolkitPortalHelperGetPipeWireFD(helper_handle);
            obj["pw_node_id"] = MaaToolkitPortalHelperGetPipeWireNodeID(helper_handle);
            MaaToolkitPortalHelperDestroy(helper_handle);
        }
    }

    if (raw.input == MaaLinuxInputMethod_UInput) {
        obj["uinput_screen_width"] = raw.uinput_screen_width;
        obj["uinput_screen_height"] = raw.uinput_screen_height;
    }

    if (raw.input == MaaLinuxInputMethod_Libei) {
        obj["eis_socket_path"] = raw.eis_socket_path;
    }

    return obj.dumps();
}

bool Runner::run(const RuntimeParam& param)
{
    if (!run_pretasks(param.pretask)) {
        return false;
    }

    MaaController* controller_handle = nullptr;
    if (const auto* p_adb_param = std::get_if<RuntimeParam::AdbParam>(&param.controller_param)) {
        RuntimeParam::AdbParam adb_param = reconfig_adb(*p_adb_param);
        controller_handle = MaaAdbControllerCreate(
            adb_param.adb_path.c_str(),
            adb_param.address.c_str(),
            adb_param.screencap,
            adb_param.input,
            adb_param.config.c_str(),
            adb_param.agent_path.c_str());
    }
    else if (const auto* p_win32_param = std::get_if<RuntimeParam::Win32Param>(&param.controller_param)) {
        controller_handle =
            MaaWin32ControllerCreate(p_win32_param->hwnd, p_win32_param->screencap, p_win32_param->mouse, p_win32_param->keyboard);
    }
    else if (const auto* p_playcover_param = std::get_if<RuntimeParam::PlayCoverParam>(&param.controller_param)) {
#if defined(__APPLE__)
        controller_handle = MaaPlayCoverControllerCreate(p_playcover_param->address.c_str(), p_playcover_param->uuid.c_str());
#else
        std::ignore = p_playcover_param;
        LogError << "PlayCover controller is only supported on macOS";
        return false;
#endif
    }
    else if (const auto* p_gamepad_param = std::get_if<RuntimeParam::GamepadParam>(&param.controller_param)) {
#if defined(_WIN32)
        controller_handle = MaaGamepadControllerCreate(p_gamepad_param->hwnd, p_gamepad_param->gamepad_type, p_gamepad_param->screencap);
#else
        std::ignore = p_gamepad_param;
        LogError << "Gamepad controller is only supported on Windows";
        return false;
#endif
    }
    else if (const auto* p_macos_param = std::get_if<RuntimeParam::MacOSParam>(&param.controller_param)) {
#if defined(__APPLE__)
        controller_handle = MaaMacOSControllerCreate(p_macos_param->window_id, p_macos_param->screencap, p_macos_param->input);
#else
        std::ignore = p_macos_param;
        LogError << "MacOS controller is only supported on macOS";
        return false;
#endif
    }
    else if (const auto* p_linux_param = std::get_if<RuntimeParam::LinuxParam>(&param.controller_param)) {
#if defined(__linux__)
        auto config_json = reconfig_linux(*p_linux_param);
        if (config_json.empty()) {
            LogError << "Failed to build Linux controller config";
            return false;
        }
        controller_handle = MaaLinuxControllerCreate(config_json.c_str());
#else
        std::ignore = p_linux_param;
        LogError << "Linux controller is only supported on Linux";
        return false;
#endif
    }
    else {
        LogError << "Unknown controller type";
        return false;
    }

    if (!controller_handle) {
        LogError << "Failed to create controller";
        return false;
    }

    MaaTasker* tasker_handle = MaaTaskerCreate();
    MaaResource* resource_handle = MaaResourceCreate();

    OnScopeLeave([&]() {
        MaaTaskerDestroy(tasker_handle);
        MaaResourceDestroy(resource_handle);
        MaaControllerDestroy(controller_handle);
    });

    // 设置分辨率选项
    if (param.display_config.raw) {
        MaaBool raw = true;
        MaaControllerSetOption(controller_handle, MaaCtrlOption_ScreenshotUseRawSize, &raw, sizeof(raw));
    }
    else if (param.display_config.expand.has_value()) {
        int32_t expand[2] = { param.display_config.expand->at(0), param.display_config.expand->at(1) };
        MaaControllerSetOption(controller_handle, MaaCtrlOption_ScreenshotTargetExpand, expand, sizeof(expand));
    }
    else if (param.display_config.long_side.has_value()) {
        int long_side = param.display_config.long_side.value();
        MaaControllerSetOption(controller_handle, MaaCtrlOption_ScreenshotTargetLongSide, &long_side, sizeof(long_side));
    }
    else if (param.display_config.short_side.has_value()) {
        int short_side = param.display_config.short_side.value();
        MaaControllerSetOption(controller_handle, MaaCtrlOption_ScreenshotTargetShortSide, &short_side, sizeof(short_side));
    }
    // 如果都没设置，使用默认值 720
    else {
        int short_side = 720;
        MaaControllerSetOption(controller_handle, MaaCtrlOption_ScreenshotTargetShortSide, &short_side, sizeof(short_side));
    }

    MaaId cid = controller_handle->post_connection();
    const auto primary_count = std::min(param.primary_resource_count, param.resource_path.size());
    const auto primary_end = param.resource_path.begin() + static_cast<std::ptrdiff_t>(primary_count);

    auto post_resources = [resource_handle](const auto& paths) {
        MaaId rid = 0;
        for (const auto& path : paths) {
            rid = resource_handle->post_bundle(path);
        }
        return rid;
    };

    MaaId rid = post_resources(std::ranges::subrange(param.resource_path.begin(), primary_end));
    if (rid != 0) {
        if (MaaStatus_Failed == resource_handle->wait(rid)) {
            LogError << "Failed to load resource";
            return false;
        }
    }

    // PI v2.6.0: 校验值基于 resource.path，必须在 attach_resource_path 之前获取。
    if (!param.resource_hash.empty()) {
        auto expected_hash = param.resource_hash;
        auto actual_hash = resource_handle->get_hash();
        tolowers_(expected_hash);
        tolowers_(actual_hash);

        if (expected_hash != actual_hash) {
            LogWarn << "Resource hash mismatch" << VAR(param.resource_hash) << VAR(actual_hash);
        }
    }

    rid = post_resources(std::ranges::subrange(primary_end, param.resource_path.end()));
    if (rid != 0 && MaaStatus_Failed == resource_handle->wait(rid)) {
        LogError << "Failed to load resource";
        return false;
    }

    tasker_handle->bind_controller(controller_handle);
    tasker_handle->bind_resource(resource_handle);

    if (MaaStatus_Failed == controller_handle->wait(cid)) {
        LogError << "Failed to connect controller";
        return false;
    }

    if (MaaStatus_Failed == resource_handle->wait(rid)) {
        LogError << "Failed to load resource";
        return false;
    }

    std::vector<MaaAgentClient*> agents;
    std::vector<std::unique_ptr<AgentProcess>> agent_children;
    for (const auto& agent_param : param.agent) {
        MaaAgentClient* agent = MaaAgentClientCreateV2(nullptr);
        MaaAgentClientBindResource(agent, resource_handle);
        auto* id_buffer = MaaStringBufferCreate();
        MaaAgentClientIdentifier(agent, id_buffer);
        std::string socket_id = MaaStringBufferGet(id_buffer);
        MaaStringBufferDestroy(id_buffer);

        std::vector<std::string> args = agent_param.child_args;
        args.emplace_back(socket_id);
        auto os_args = conv_args(args);

        // v2.5.0: set PI_* environment variables in current process (child inherits them)
        for (const auto& [key, val] : agent_param.env_vars) {
#ifdef _WIN32
            SetEnvironmentVariableW(to_u16(key).c_str(), to_u16(val).c_str());
#else
            setenv(key.c_str(), val.c_str(), 1);
#endif
        }

        LogInfo << "Start Agent" << VAR(agent_param.child_exec) << VAR(os_args) << VAR(agent_param.cwd);
        auto agent_child = spawn_agent(agent_param.child_exec, os_args, agent_param.cwd);
        if (!agent_child || !agent_child->valid()) {
            LogError << "Failed to start agent process" << VAR(agent_param.child_exec) << VAR(args) << VAR(agent_param.cwd);
            return false;
        }
        agent_children.emplace_back(std::move(agent_child));

        bool connected = MaaAgentClientConnect(agent);
        if (!connected) {
            LogError << "Failed to connect agent" << VAR(agent_param.child_exec) << VAR(args);
            return false;
        }

        agents.emplace_back(agent);
    }

    MaaId tid = 0;
    for (const auto& task : param.task) {
        tid = tasker_handle->post_task(task.entry, task.pipeline_override);
    }

    tasker_handle->wait(tid);

    for (auto* agent : agents) {
        MaaAgentClientDisconnect(agent);
        MaaAgentClientDestroy(agent);
    }

    return true;
}

bool Runner::run_pretasks(const std::vector<RuntimeParam::Pretask>& pretasks)
{
    for (const auto& pretask : pretasks) {
        LogInfo << "Run pretask" << VAR(pretask.name) << VAR(pretask.exec) << VAR(pretask.cwd);
        if (!run_pretask_process(pretask)) {
            LogError << "Pretask failed" << VAR(pretask.name) << VAR(pretask.exec);
            return false;
        }
    }

    return true;
}

MAA_PROJECT_INTERFACE_NS_END
