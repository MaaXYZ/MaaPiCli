#include "interactor.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <ranges>
#include <unordered_set>

#include <boost/regex.hpp>

#if defined(_WIN32)
#include "MaaUtils/SafeWindows.hpp"
#include <shellapi.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include "MaaFramework/Utility/MaaBuffer.h"
#include "MaaToolkit/AdbDevice/MaaToolkitAdbDevice.h"
#include "MaaToolkit/DesktopWindow/MaaToolkitDesktopWindow.h"
#include "MaaUtils/Encoding.h"
#include "MaaUtils/Logger.h"
#include "MaaUtils/Platform.h"
#include "ProjectInterface/Parser.h"
#include "ProjectInterface/Runner.h"
#include "input.h"

#if defined(__APPLE__)
static constexpr bool kPlayCoverSupported = true;
#else
static constexpr bool kPlayCoverSupported = false;
#endif

#if defined(_WIN32)
static constexpr bool kGamepadSupported = true;
#else
static constexpr bool kGamepadSupported = false;
#endif

#if defined(__linux__)
static constexpr bool kLinuxSupported = true;
#else
static constexpr bool kLinuxSupported = false;
#endif

std::string normalize_linux_screencap_method(const std::string& method)
{
    static const std::unordered_set<std::string> known_methods = { "Wlr", "PipeWire" };
    return method.empty() || !known_methods.contains(method) ? "Wlr" : method;
}

std::string normalize_linux_input_method(const std::string& method)
{
    static const std::unordered_set<std::string> known_methods = { "Wlr", "UInput", "Libei" };
    return method.empty() || !known_methods.contains(method) ? "Wlr" : method;
}

std::optional<std::string> read_hidden_line()
{
#ifdef _WIN32
    const HANDLE console =
        CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (console == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    DWORD original_mode = 0;
    if (!GetConsoleMode(console, &original_mode)) {
        CloseHandle(console);
        return std::nullopt;
    }

    std::wstring buffer(4096, L'\0');
    DWORD read = 0;
    CONSOLE_READCONSOLE_CONTROL control { .nLength = sizeof(control), .nInitialChars = 0, .dwCtrlWakeupMask = 0, .dwControlKeyState = 0 };
    const BOOL success = SetConsoleMode(console, original_mode & ~ENABLE_ECHO_INPUT)
                         && ReadConsoleW(console, buffer.data(), static_cast<DWORD>(buffer.size()), &read, &control);
    SetConsoleMode(console, original_mode);
    CloseHandle(console);
    if (!success) {
        return std::nullopt;
    }

    buffer.resize(read);
    while (!buffer.empty() && (buffer.back() == L'\r' || buffer.back() == L'\n')) {
        buffer.pop_back();
    }
    return MAA_NS::from_u16(buffer);
#else
    termios original { };
    if (tcgetattr(STDIN_FILENO, &original) != 0) {
        return std::nullopt;
    }

    termios hidden = original;
    hidden.c_lflag &= ~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &hidden) != 0) {
        return std::nullopt;
    }

    std::string line;
    std::getline(std::cin, line);
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    if (!std::cin) {
        return std::nullopt;
    }
    return line;
#endif
}

void clear_screen()
{
#ifdef _WIN32
    std::ignore = system("cls");
#else
    std::ignore = system("clear");
#endif
}

namespace
{
#if defined(_WIN32)
// Windows path limit: MAX_PATH (260) is insufficient for long paths; use a larger buffer.
// Modern Windows supports paths up to 32,767 characters with proper configuration.
constexpr DWORD kMaxPathBuffer = 32768;

// ShellExecuteW return value threshold: values > 32 indicate success, <= 32 indicate error codes.
// See: https://docs.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
constexpr intptr_t kShellExecuteSuccessThreshold = 32;

bool is_running_as_admin()
{
    BOOL is_member = FALSE;

    // CheckTokenMembership(nullptr, ...) checks the current effective token (UAC-aware).
    BYTE sid_buffer[SECURITY_MAX_SID_SIZE] = { };
    DWORD sid_size = sizeof(sid_buffer);
    PSID admin_sid = sid_buffer;

    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admin_sid, &sid_size)) {
        return false;
    }
    if (!CheckTokenMembership(nullptr, admin_sid, &is_member)) {
        return false;
    }
    return is_member == TRUE;
}

std::optional<std::wstring> get_current_exe_path()
{
    std::wstring buf;
    buf.resize(kMaxPathBuffer);
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) {
        return std::nullopt;
    }
    buf.resize(len);
    return buf;
}

bool restart_self_as_admin()
{
    auto exe = get_current_exe_path();
    if (!exe) {
        return false;
    }

    // Preserve original command-line arguments (excluding argv[0], the executable path)
    std::wstring combined_params;
    int argc = 0;
    LPWSTR cmd_line = GetCommandLineW();
    LPWSTR* argv = nullptr;

    if (cmd_line != nullptr) {
        argv = CommandLineToArgvW(cmd_line, &argc);
    }

    if (argv != nullptr && argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (!combined_params.empty()) {
                combined_params.push_back(L' ');
            }
            combined_params.append(argv[i]);
        }
        LocalFree(argv);
    }

    const wchar_t* lpParameters = combined_params.empty() ? nullptr : combined_params.c_str();

    const auto ret = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"runas", exe->c_str(), lpParameters, nullptr, SW_SHOWNORMAL));
    return ret > kShellExecuteSuccessThreshold;
}
#endif
} // namespace

Interactor::Interactor(std::filesystem::path user_path)
    : user_path_(std::move(user_path))
{
    LogDebug << VAR(user_path_);
}

bool Interactor::load(const std::filesystem::path& resource_path)
{
    LogFunc << VAR(resource_path);

    if (!config_.load(resource_path, user_path_)) {
        mpause();
        return false;
    }

    if (!config_.check_configuration()) {
        std::cout << "### The interface has changed and incompatible configurations have been "
                     "deleted. ###\n\n";
        mpause();
    }

    return true;
}

bool Interactor::interact()
{
    if (config_.is_first_time_use()) {
        if (!interact_for_first_time_use()) {
            return input_aborted_;
        }
        if (!save_config()) {
            return false;
        }
    }

    while (true) {
        print_config();
        const auto status = interact_once();
        if (status == ActionStatus::Aborted || status == ActionStatus::Exit) {
            return true;
        }
        if (status == ActionStatus::Incomplete) {
            continue;
        }
        if (!save_config()) {
            return false;
        }
    }
}

const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller* Interactor::find_current_controller() const
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& name = config_.configuration().controller.name;
    auto it = std::ranges::find(config_.interface_data().controller, name, std::mem_fn(&InterfaceData::Controller::name));

    return (it != config_.interface_data().controller.end()) ? &(*it) : nullptr;
}

Interactor::ElevationResult Interactor::check_and_elevate_if_needed()
{
#if defined(_WIN32)
    const auto* controller = find_current_controller();
    if (!controller || !controller->permission_required || is_running_as_admin()) {
        return ElevationResult::NotNeeded;
    }

    std::cout << "\nThis controller requires administrator privileges.\n"
                 "MaaPiCli will try to restart itself as Administrator to run tasks (UAC prompt will appear).\n\n";

    if (!save_config()) {
        return ElevationResult::Failed;
    }

    if (!restart_self_as_admin()) {
        std::cout << "\nFailed to restart as Administrator (UAC may have been cancelled, or the request was denied).\n"
                     "Please manually start MaaPiCli as Administrator and run again.\n\n";
        return ElevationResult::Failed;
    }

    // Elevated instance has been started; caller should exit current process.
    return ElevationResult::ElevatedStarted;
#else
    return ElevationResult::NotNeeded;
#endif
}

bool Interactor::run()
{
    auto elevation_result = check_and_elevate_if_needed();
    if (elevation_result == ElevationResult::Failed) {
        return false;
    }
    if (elevation_result == ElevationResult::ElevatedStarted) {
        // Elevated instance is now running; signal caller to exit gracefully.
        return true;
    }

    if (!check_validity()) {
        LogError << "Config is invalid";
        return false;
    }

    if (!ensure_runtime_options()) {
        return false;
    }

    if (!ensure_pretask_options()) {
        return false;
    }
    if (!save_config()) {
        return false;
    }

    auto runtime = config_.generate_runtime();
    if (!runtime) {
        LogError << "Failed to generate runtime";
        return false;
    }

    bool ret = MAA_PROJECT_INTERFACE_NS::Runner::run(runtime.value());

    if (!ret) {
        std::cout << "### Failed to run tasks ###\n\n";
    }
    else {
        std::cout << "### All tasks have been completed ###\n\n";
    }

    return ret;
}

void Interactor::print_config() const
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    clear_screen();

    welcome();
    std::cout << "### Current configuration ###\n\n";

    std::cout << "Controller:\n\n";
    std::cout << "\t" << MAA_NS::utf8_to_crt(config_.configuration().controller.name) << "\n";

    switch (config_.configuration().controller.type) {
    case InterfaceData::Controller::Type::Adb:
        std::cout << MAA_NS::utf8_to_crt(
            std::format("\t\t{}\n\t\t{}\n", config_.configuration().adb.adb_path, config_.configuration().adb.address));
        break;
    case InterfaceData::Controller::Type::Win32:
        if (config_.configuration().win32.hwnd) {
            std::cout << MAA_NS::utf8_to_crt(std::format("\t\t{}\n", format_win32_config(config_.configuration().win32)));
        }
        break;
    case InterfaceData::Controller::Type::MacOS: {
        const auto& macos = config_.configuration().macos;
        std::cout << MAA_NS::utf8_to_crt(
            std::format(
                "\t\tWindow ID: {}\n\t\tTitle: {}\n\t\tScreencap: {}\n\t\tInput: {}\n",
                macos.window_id,
                macos.title,
                macos.screencap,
                macos.input));
    } break;
    case InterfaceData::Controller::Type::PlayCover: {
        const auto& pc = config_.configuration().playcover;
        if (!pc.address.empty() && !pc.uuid.empty()) {
            std::cout << MAA_NS::utf8_to_crt(std::format("\t\t{}\n\t\t{}\n", pc.address, pc.uuid));
        }
        if (!kPlayCoverSupported) {
            std::cout << "\t\t(PlayCover is only available on macOS)\n";
        }
    } break;
    case InterfaceData::Controller::Type::Gamepad: {
        if (config_.configuration().gamepad.hwnd) {
            std::cout << MAA_NS::utf8_to_crt(std::format("\t\t{}\n", format_gamepad_config(config_.configuration().gamepad)));
        }
        if (!kGamepadSupported) {
            std::cout << "\t\t(Gamepad is only available on Windows)\n";
        }
    } break;
    case InterfaceData::Controller::Type::Linux: {
        const auto& lnx = config_.configuration().lnx;
        if (!lnx.wlr_socket_path.empty()) {
            std::cout << MAA_NS::utf8_to_crt(std::format("\t\t{}\n", lnx.wlr_socket_path));
        }
        if (lnx.uinput_screen_width != 0 || lnx.uinput_screen_height != 0) {
            std::cout << MAA_NS::utf8_to_crt(std::format("\t\tUInput: {}x{}\n", lnx.uinput_screen_width, lnx.uinput_screen_height));
        }
        if (!lnx.eis_socket_path.empty()) {
            std::cout << MAA_NS::utf8_to_crt(std::format("\t\tEIS: {}\n", lnx.eis_socket_path));
        }
        if (!kLinuxSupported) {
            std::cout << "\t\t(Linux is only available on Linux)\n";
        }
    } break;
    default:
        LogError << "Unknown controller type" << VAR(config_.configuration().controller.type);
        break;
    }

    std::cout << "\n";

    std::cout << "Resource:\n\n";
    std::cout << "\t" << MAA_NS::utf8_to_crt(config_.configuration().resource) << "\n\n";

    auto print_level_options = [&](const std::string& label, const std::vector<MAA_PROJECT_INTERFACE_NS::Configuration::Option>& opts) {
        if (opts.empty()) {
            return;
        }
        std::cout << label << ":\n\n";
        for (const auto& opt : opts) {
            if (!opt.value.empty()) {
                std::cout << "\t" << MAA_NS::utf8_to_crt(opt.name) << ": " << MAA_NS::utf8_to_crt(opt.value) << "\n";
            }
            else if (!opt.values.empty()) {
                std::cout << "\t" << MAA_NS::utf8_to_crt(opt.name) << ": [";
                for (size_t j = 0; j < opt.values.size(); ++j) {
                    if (j > 0) {
                        std::cout << ", ";
                    }
                    std::cout << MAA_NS::utf8_to_crt(opt.values[j]);
                }
                std::cout << "]\n";
            }
            else if (!opt.inputs.empty()) {
                std::cout << "\t" << MAA_NS::utf8_to_crt(opt.name) << ":\n";
                for (const auto& [key, val] : opt.inputs) {
                    std::cout << "\t\t" << MAA_NS::utf8_to_crt(key) << ": " << MAA_NS::utf8_to_crt(display_input_value(opt.name, key, val))
                              << "\n";
                }
            }
        }
        std::cout << "\n";
    };

    print_level_options("Global Options", config_.configuration().global_option);
    print_level_options("Resource Options", config_.configuration().resource_option);
    print_level_options("Controller Options", config_.configuration().controller_option);

    std::cout << "Tasks:\n\n";
    print_config_tasks(false);
}

bool Interactor::interact_for_first_time_use()
{
    welcome();
    if (!select_controller()) {
        return false;
    }
    if (!select_resource()) {
        return false;
    }

    // v2.3.0: process global/resource/controller-level options
    if (!process_level_options(config_.interface_data().global_option, config_.configuration().global_option, "Global")) {
        return false;
    }

    if (auto res_it = std::ranges::find(
            config_.interface_data().resource,
            config_.configuration().resource,
            std::mem_fn(&MAA_PROJECT_INTERFACE_NS::InterfaceData::Resource::name));
        res_it != config_.interface_data().resource.end()) {
        if (!process_level_options(res_it->option, config_.configuration().resource_option, "Resource")) {
            return false;
        }
    }

    if (auto ctrl_it = std::ranges::find(
            config_.interface_data().controller,
            config_.configuration().controller.name,
            std::mem_fn(&MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::name));
        ctrl_it != config_.interface_data().controller.end()) {
        if (!process_level_options(ctrl_it->option, config_.configuration().controller_option, "Controller")) {
            return false;
        }
    }

    // Auto-add tasks with default_check=true
    if (!add_default_tasks()) {
        return false;
    }

    // If no default tasks were added, let user select manually
    if (config_.configuration().task.empty()) {
        return add_task();
    }

    return true;
}

void Interactor::welcome() const
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& data = config_.interface_data();

    // 显示标题或项目名称
    if (!data.title.empty()) {
        std::cout << MAA_NS::utf8_to_crt(config_.translate(data.title)) << "\n\n";
    }
    else {
        std::string display_name = get_display_name(data.name, data.label);
        if (!display_name.empty()) {
            std::cout << MAA_NS::utf8_to_crt(display_name);
            if (!data.version.empty()) {
                std::cout << " v" << MAA_NS::utf8_to_crt(data.version);
            }
            std::cout << "\n\n";
        }
    }

    // 显示欢迎信息
    if (!data.welcome.empty()) {
        std::string welcome_text = read_text_content(data.welcome);
        std::cout << MAA_NS::utf8_to_crt(welcome_text) << "\n\n";
    }

    // 显示项目描述
    if (!data.description.empty()) {
        std::string desc_text = read_text_content(data.description);
        std::cout << "Description: " << MAA_NS::utf8_to_crt(desc_text) << "\n\n";
    }

    // 显示 GitHub 地址
    if (!data.github.empty()) {
        std::cout << "GitHub: " << MAA_NS::utf8_to_crt(data.github) << "\n\n";
    }

    // 显示联系方式
    if (!data.contact.empty()) {
        std::string contact_text = read_text_content(data.contact);
        std::cout << "Contact: " << MAA_NS::utf8_to_crt(contact_text) << "\n\n";
    }

    // 显示许可证信息
    if (!data.license.empty()) {
        std::string license_text = read_text_content(data.license);
        std::cout << "License: " << MAA_NS::utf8_to_crt(license_text) << "\n\n";
    }
}

Interactor::ActionStatus Interactor::interact_once()
{
    bool has_presets = !config_.interface_data().preset.empty();

    std::cout << "### Select action ###\n\n";
    std::cout << "\t1. Switch controller\n";
    std::cout << "\t2. Switch resource\n";
    std::cout << "\t3. Add task\n";
    std::cout << "\t4. Move task\n";
    std::cout << "\t5. Delete task\n";
    std::cout << "\t6. Run tasks\n";
    if (has_presets) {
        std::cout << "\t7. Apply preset\n";
        std::cout << "\t8. Exit\n";
    }
    else {
        std::cout << "\t7. Exit\n";
    }
    std::cout << "\n";

    int max_action = has_presets ? 8 : 7;
    auto selected_action = input(max_action);
    if (!selected_action) {
        input_aborted_ = true;
        return ActionStatus::Aborted;
    }
    const int action = *selected_action;

    switch (action) {
    case 1:
        return action_status(select_controller());
    case 2:
        return action_status(select_resource());
    case 3:
        return action_status(add_task());
    case 4:
        return action_status(move_task());
    case 5:
        return action_status(delete_task());
    case 6: {
        const bool completed = run();
        if (!mpause()) {
            return ActionStatus::Aborted;
        }
        return completed ? ActionStatus::Complete : ActionStatus::Incomplete;
    }
    case 7:
        if (has_presets) {
            return action_status(apply_preset());
        }
        else {
            return ActionStatus::Exit;
        }
    case 8:
        if (has_presets) {
            return ActionStatus::Exit;
        }
        break;
    default:
        LogError << "Invalid action" << VAR(action);
        return ActionStatus::Incomplete;
    }

    return ActionStatus::Incomplete;
}

Interactor::ActionStatus Interactor::action_status(bool completed) const
{
    if (completed) {
        return ActionStatus::Complete;
    }
    return input_aborted_ ? ActionStatus::Aborted : ActionStatus::Incomplete;
}

bool Interactor::select_controller()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& all_controllers = config_.interface_data().controller;

    if (all_controllers.empty()) {
        LogError << "Controller is empty";
        return false;
    }

    int index = 0;
    if (all_controllers.size() != 1) {
        std::cout << "### Select controller ###\n\n";
        for (size_t i = 0; i < all_controllers.size(); ++i) {
            const auto& ctrl = all_controllers[i];
            std::string display_name = get_display_name(ctrl.name, ctrl.label);
            std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}", i + 1, display_name));
            if (ctrl.type == InterfaceData::Controller::Type::PlayCover && !kPlayCoverSupported) {
                std::cout << " (macOS only)";
            }
            if (ctrl.type == InterfaceData::Controller::Type::Gamepad && !kGamepadSupported) {
                std::cout << " (Windows only)";
            }
            std::cout << "\n";
            if (!ctrl.description.empty()) {
                std::string desc_text = read_text_content(ctrl.description);
                std::cout << "\t   " << MAA_NS::utf8_to_crt(desc_text) << "\n";
            }
        }
        std::cout << "\n";
        auto selected = input(all_controllers.size());
        if (!selected) {
            input_aborted_ = true;
            return false;
        }
        index = *selected - 1;
    }
    else {
        index = 0;
    }
    const auto& controller = all_controllers[index];

    config_.configuration().controller.name = controller.name;
    config_.configuration().controller.type = controller.type;

    switch (controller.type) {
    case InterfaceData::Controller::Type::Adb:
        config_.configuration().controller.type = InterfaceData::Controller::Type::Adb;
        return select_adb();
    case InterfaceData::Controller::Type::Win32:
        config_.configuration().controller.type = InterfaceData::Controller::Type::Win32;
        return select_win32_hwnd(controller.win32);
    case InterfaceData::Controller::Type::MacOS:
        config_.configuration().controller.type = InterfaceData::Controller::Type::MacOS;
        return select_macos(controller.macos);
    case InterfaceData::Controller::Type::PlayCover:
        if (!kPlayCoverSupported) {
            std::cout << "\nPlayCover controller is only available on macOS.\n";
            // Check if there are other controllers available
            bool has_other_controllers = std::ranges::any_of(all_controllers, [](const auto& ctrl) {
                return ctrl.type != InterfaceData::Controller::Type::PlayCover;
            });
            if (has_other_controllers) {
                std::cout << "Please select another controller.\n\n";
                return mpause() && select_controller();
            }
            else {
                std::cout << "No other controllers available.\n\n";
                return mpause();
            }
        }
        config_.configuration().controller.type = InterfaceData::Controller::Type::PlayCover;
        return select_playcover(controller.playcover);
    case InterfaceData::Controller::Type::Linux:
        if (!kLinuxSupported) {
            std::cout << "\nLinux controller is only available on Linux.\n";
            // Check if there are other controllers available
            bool has_other_controllers =
                std::ranges::any_of(all_controllers, [](const auto& ctrl) { return ctrl.type != InterfaceData::Controller::Type::Linux; });
            if (has_other_controllers) {
                std::cout << "Please select another controller.\n\n";
                return mpause() && select_controller();
            }
            else {
                std::cout << "No other controllers available.\n\n";
                return mpause();
            }
        }
        config_.configuration().controller.type = InterfaceData::Controller::Type::Linux;
        return select_linux(controller.lnx);
    case InterfaceData::Controller::Type::Gamepad:
        if (!kGamepadSupported) {
            std::cout << "\nGamepad controller is only available on Windows.\n";
            // Check if there are other controllers available
            bool has_other_controllers = std::ranges::any_of(all_controllers, [](const auto& ctrl) {
                return ctrl.type != InterfaceData::Controller::Type::Gamepad;
            });
            if (has_other_controllers) {
                std::cout << "Please select another controller.\n\n";
                return mpause() && select_controller();
            }
            else {
                std::cout << "No other controllers available.\n\n";
                return mpause();
            }
        }
        config_.configuration().controller.type = InterfaceData::Controller::Type::Gamepad;
        return select_gamepad(controller.gamepad);
    default:
        LogError << "Unknown controller type" << VAR(controller.type);
        return false;
    }

    return true;
}

bool Interactor::select_adb()
{
    std::cout << "### Select ADB ###\n\n";

    std::cout << "\t1. Auto detect\n";
    std::cout << "\t2. Manual input\n";
    std::cout << "\n";

    auto selected = input(2);
    if (!selected) {
        input_aborted_ = true;
        return false;
    }

    switch (*selected) {
    case 1:
        return select_adb_auto_detect();

    case 2:
        return select_adb_manual_input();
    }

    return false;
}

bool Interactor::select_adb_auto_detect()
{
    std::cout << "Finding device...\n\n";

    auto list_handle = MaaToolkitAdbDeviceListCreate();
    OnScopeLeave([&]() { MaaToolkitAdbDeviceListDestroy(list_handle); });

    MaaToolkitAdbDeviceFind(list_handle);

    size_t size = MaaToolkitAdbDeviceListSize(list_handle);
    if (size == 0) {
        std::cout << "No device found!\n\n";
        return select_adb();
    }

    std::cout << "## Select Device ##\n\n";

    for (size_t i = 0; i < size; ++i) {
        auto device_handle = MaaToolkitAdbDeviceListAt(list_handle, i);

        std::string name = MaaToolkitAdbDeviceGetName(device_handle);
        std::string path = MaaToolkitAdbDeviceGetAdbPath(device_handle);
        std::string address = MaaToolkitAdbDeviceGetAddress(device_handle);

        std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}\n\t\t{}\n\t\t{}\n", i + 1, name, path, address));
    }
    std::cout << "\n";

    auto selected = input(size);
    if (!selected) {
        input_aborted_ = true;
        return false;
    }
    const size_t index = static_cast<size_t>(*selected - 1);
    auto& adb_config = config_.configuration().adb;

    auto device_handle = MaaToolkitAdbDeviceListAt(list_handle, index);

    adb_config.name = MaaToolkitAdbDeviceGetName(device_handle);
    adb_config.adb_path = MaaToolkitAdbDeviceGetAdbPath(device_handle);
    adb_config.address = MaaToolkitAdbDeviceGetAddress(device_handle);

    return true;
}

bool Interactor::select_adb_manual_input()
{
    auto adb_path = read_line("Please input ADB path: ");
    if (!adb_path) {
        input_aborted_ = true;
        return false;
    }
    config_.configuration().adb.adb_path = *adb_path;
    std::cout << "\n";

    auto adb_address = read_line("Please input ADB address: ");
    if (!adb_address) {
        input_aborted_ = true;
        return false;
    }
    config_.configuration().adb.address = *adb_address;
    std::cout << "\n";

    config_.configuration().adb.name = std::format("{}-{}", *adb_address, *adb_path);

    return true;
}

bool Interactor::select_playcover(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::PlayCoverConfig& playcover_config)
{
    std::cout << "### Configure PlayCover ###\n\n";

    auto& pc = config_.configuration().playcover;

    // Use uuid from interface.json if available
    if (!playcover_config.uuid.empty() && pc.uuid.empty()) {
        pc.uuid = playcover_config.uuid;
    }

    // Default address if not configured
    std::string default_address = pc.address.empty() ? "127.0.0.1:1717" : pc.address;

    // Ask for address (use default if empty input)
    auto buffer = read_line(std::format("PlayTools service address (host:port) [{}]: ", default_address));
    if (!buffer) {
        input_aborted_ = true;
        return false;
    }

    pc.address = buffer->empty() ? default_address : *buffer;
    std::cout << "\n";

    return true;
}

std::string Interactor::format_win32_config(const MAA_PROJECT_INTERFACE_NS::Configuration::Win32Config& win32_config)
{
    return std::format(
        "{}\n\t\t{}\n\t\t{}",
        win32_config.hwnd,
        MAA_NS::from_u16(win32_config.class_name),
        MAA_NS::from_u16(win32_config.window_name));
}

std::string Interactor::format_gamepad_config(const MAA_PROJECT_INTERFACE_NS::Configuration::GamepadConfig& gamepad_config)
{
    std::string type_str = gamepad_config.gamepad_type.empty() ? "Xbox360" : gamepad_config.gamepad_type;
    return std::format(
        "{}\n\t\t{}\n\t\t{}\n\t\tGamepad: {}",
        gamepad_config.hwnd,
        MAA_NS::from_u16(gamepad_config.class_name),
        MAA_NS::from_u16(gamepad_config.window_name),
        type_str);
}

bool Interactor::select_win32_hwnd(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::Win32Config& win32_config)
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    auto list_handle = MaaToolkitDesktopWindowListCreate();
    OnScopeLeave([&]() { MaaToolkitDesktopWindowListDestroy(list_handle); });

    MaaToolkitDesktopWindowFindAll(list_handle);

    size_t list_size = MaaToolkitDesktopWindowListSize(list_handle);

    auto class_regex = MAA_NS::regex_valid(MAA_NS::to_u16(win32_config.class_regex));
    auto window_regex = MAA_NS::regex_valid(MAA_NS::to_u16(win32_config.window_regex));
    if (!class_regex || !window_regex) {
        LogError << "regex is invalid" << VAR(win32_config.class_regex) << VAR(win32_config.window_regex);
        return false;
    }

    std::vector<Configuration::Win32Config> matched_config;
    for (size_t i = 0; i < list_size; ++i) {
        Configuration::Win32Config rt_config;

        auto window_handle = MaaToolkitDesktopWindowListAt(list_handle, i);
        rt_config.hwnd = MaaToolkitDesktopWindowGetHandle(window_handle);
        rt_config.class_name = MAA_NS::to_u16(MaaToolkitDesktopWindowGetClassName(window_handle));
        rt_config.window_name = MAA_NS::to_u16(MaaToolkitDesktopWindowGetWindowName(window_handle));

        if (boost::regex_search(rt_config.class_name, *class_regex) && boost::regex_search(rt_config.window_name, *window_regex)) {
            matched_config.emplace_back(std::move(rt_config));
        }
    }

    if (matched_config.empty()) {
        LogError << "Window Not Found" << VAR(win32_config.class_regex) << VAR(win32_config.window_regex);
        mpause();
        return false;
    }
    size_t matched_size = matched_config.size();
    if (matched_size == 1) {
        config_.configuration().win32 = matched_config.front();
        return true;
    }

    std::cout << "### Select HWND ###\n\n";

    for (size_t i = 0; i < matched_size; ++i) {
        std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}\n", i + 1, format_win32_config(matched_config.at(i))));
    }
    std::cout << "\n";

    auto selected = input(matched_size);
    if (!selected) {
        input_aborted_ = true;
        return false;
    }
    const size_t index = static_cast<size_t>(*selected - 1);
    config_.configuration().win32 = matched_config.at(index);

    return true;
}

bool Interactor::select_gamepad(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::GamepadConfig& gamepad_config)
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    std::cout << "### Configure Gamepad Controller ###\n\n";

    // Select window for screencap (optional, reuse Win32 window selection logic)
    auto list_handle = MaaToolkitDesktopWindowListCreate();
    OnScopeLeave([&]() { MaaToolkitDesktopWindowListDestroy(list_handle); });

    MaaToolkitDesktopWindowFindAll(list_handle);

    size_t list_size = MaaToolkitDesktopWindowListSize(list_handle);

    auto class_regex = MAA_NS::regex_valid(MAA_NS::to_u16(gamepad_config.class_regex));
    auto window_regex = MAA_NS::regex_valid(MAA_NS::to_u16(gamepad_config.window_regex));
    if (!class_regex || !window_regex) {
        LogError << "regex is invalid" << VAR(gamepad_config.class_regex) << VAR(gamepad_config.window_regex);
        std::cout << "Window regex is invalid, screencap will not be available.\n";
        config_.configuration().gamepad.hwnd = nullptr;
    }
    else {
        std::vector<Configuration::GamepadConfig> matched_config;
        for (size_t i = 0; i < list_size; ++i) {
            Configuration::GamepadConfig rt_config;

            auto window_handle = MaaToolkitDesktopWindowListAt(list_handle, i);
            rt_config.hwnd = MaaToolkitDesktopWindowGetHandle(window_handle);
            rt_config.class_name = MAA_NS::to_u16(MaaToolkitDesktopWindowGetClassName(window_handle));
            rt_config.window_name = MAA_NS::to_u16(MaaToolkitDesktopWindowGetWindowName(window_handle));

            if (boost::regex_search(rt_config.class_name, *class_regex) && boost::regex_search(rt_config.window_name, *window_regex)) {
                matched_config.emplace_back(std::move(rt_config));
            }
        }

        if (matched_config.empty()) {
            std::cout << "No matching window found, screencap will not be available.\n";
            config_.configuration().gamepad.hwnd = nullptr;
        }
        else {
            size_t matched_size = matched_config.size();
            if (matched_size == 1) {
                config_.configuration().gamepad.hwnd = matched_config.front().hwnd;
                config_.configuration().gamepad.class_name = matched_config.front().class_name;
                config_.configuration().gamepad.window_name = matched_config.front().window_name;
            }
            else {
                std::cout << "### Select HWND for screencap ###\n\n";

                for (size_t i = 0; i < matched_size; ++i) {
                    std::cout << MAA_NS::utf8_to_crt(
                        std::format(
                            "\t{}. {}\n\t\t{}\n\t\t{}\n",
                            i + 1,
                            matched_config.at(i).hwnd,
                            MAA_NS::from_u16(matched_config.at(i).class_name),
                            MAA_NS::from_u16(matched_config.at(i).window_name)));
                }
                std::cout << "\n";

                auto selected = input(matched_size);
                if (!selected) {
                    input_aborted_ = true;
                    return false;
                }
                const auto& window = matched_config.at(static_cast<size_t>(*selected - 1));
                config_.configuration().gamepad.hwnd = window.hwnd;
                config_.configuration().gamepad.class_name = window.class_name;
                config_.configuration().gamepad.window_name = window.window_name;
            }
        }
    }

    // Select gamepad type
    std::cout << "\n### Select Gamepad Type ###\n\n";
    std::cout << "\t1. Xbox 360\n";
    std::cout << "\t2. DualShock 4 (PS4)\n";
    std::cout << "\n";

    auto type_index = input(2);
    if (!type_index) {
        input_aborted_ = true;
        return false;
    }
    config_.configuration().gamepad.gamepad_type = (*type_index == 1) ? "Xbox360" : "DualShock4";

    std::cout << "\n";

    return true;
}

bool Interactor::select_macos(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::MacOSConfig& macos_config)
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    std::cout << "### Configure macOS Controller ###\n\n";

    auto& mac = config_.configuration().macos;

    // Select window using title_regex
    std::string title_regex_str = macos_config.title_regex;
    if (title_regex_str.empty()) {
        auto input_regex = read_line("Title regex: ");
        if (!input_regex) {
            input_aborted_ = true;
            return false;
        }
        title_regex_str = std::move(*input_regex);
    }

    if (!title_regex_str.empty()) {
        auto title_regex = MAA_NS::regex_valid(MAA_NS::to_u16(title_regex_str));
        if (title_regex) {
            auto list_handle = MaaToolkitDesktopWindowListCreate();
            OnScopeLeave([&]() { MaaToolkitDesktopWindowListDestroy(list_handle); });

            MaaToolkitDesktopWindowFindAll(list_handle);

            size_t list_size = MaaToolkitDesktopWindowListSize(list_handle);

            std::vector<std::pair<uint32_t, std::string>> matched_windows;
            for (size_t i = 0; i < list_size; ++i) {
                auto window_handle = MaaToolkitDesktopWindowListAt(list_handle, i);
                std::string window_name = MaaToolkitDesktopWindowGetWindowName(window_handle);

                if (boost::regex_search(MAA_NS::to_u16(window_name), *title_regex)) {
                    void* hwnd = MaaToolkitDesktopWindowGetHandle(window_handle);
                    // On macOS, hwnd is (void*)(uintptr_t)windowID, so extract uint32_t
                    uint32_t window_id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hwnd));
                    matched_windows.emplace_back(window_id, window_name);
                }
            }

            if (!matched_windows.empty()) {
                if (matched_windows.size() == 1) {
                    mac.window_id = matched_windows.front().first;
                    mac.title = matched_windows.front().second;
                    std::cout << "Auto-selected window ID: " << mac.window_id << "\n\n";
                }
                else {
                    std::cout << "### Select Window ###\n\n";
                    for (size_t i = 0; i < matched_windows.size(); ++i) {
                        std::cout << MAA_NS::utf8_to_crt(
                            std::format("\t{}. Window ID: {} - {}\n", i + 1, matched_windows[i].first, matched_windows[i].second));
                    }
                    std::cout << "\n";

                    auto selected = input(matched_windows.size());
                    if (!selected) {
                        input_aborted_ = true;
                        return false;
                    }
                    const auto& window = matched_windows.at(static_cast<size_t>(*selected - 1));
                    mac.window_id = window.first;
                    mac.title = window.second;
                }
            }
            else {
                LogWarn << "No window matched regex" << VAR(title_regex_str);
                std::cout << "No window found matching regex.\n\n";
                return false;
            }
        }
        else {
            LogError << "Invalid title regex" << VAR(title_regex_str);
            return false;
        }
    }
    else {
        std::cout << "Title regex is required.\n\n";
        return false;
    }

    // Use screencap_method from interface.json if available
    if (!macos_config.screencap.empty() && mac.screencap.empty()) {
        mac.screencap = macos_config.screencap;
    }

    // Use input_method from interface.json if available
    if (!macos_config.input.empty() && mac.input.empty()) {
        mac.input = macos_config.input;
    }

    // Default values
    std::string default_screencap = mac.screencap.empty() ? "ScreenCaptureKit" : mac.screencap;
    std::string default_input = mac.input.empty() ? "GlobalEvent" : mac.input;

    // Ask for screencap_method
    auto buffer = read_line(std::format("Screencap method [{}]: ", default_screencap));
    if (!buffer) {
        input_aborted_ = true;
        return false;
    }

    mac.screencap = buffer->empty() ? default_screencap : *buffer;

    // Ask for input_method
    buffer = read_line(std::format("Input method [{}]: ", default_input));
    if (!buffer) {
        input_aborted_ = true;
        return false;
    }

    mac.input = buffer->empty() ? default_input : *buffer;
    std::cout << "\n";

    return true;
}

bool Interactor::select_linux(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::LinuxConfig& linux_config)
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    std::cout << "### Configure Linux Controller ###\n\n";

    auto& lnx = config_.configuration().lnx;

    const std::string screencap = normalize_linux_screencap_method(linux_config.screencap);
    const std::string input = normalize_linux_input_method(linux_config.input);

    if (screencap == "Wlr" || input == "Wlr") {
        if (!select_wlroots()) {
            return false;
        }
    }

    if (input == "Libei") {
        const std::string default_eis = lnx.eis_socket_path;
        while (true) {
            auto socket_path = read_line(std::format("EIS socket path (e.g. /run/user/1000/gamescope-0-ei) [{}]: ", default_eis));
            if (!socket_path) {
                input_aborted_ = true;
                return false;
            }

            if (!socket_path->empty()) {
                lnx.eis_socket_path = std::move(*socket_path);
                break;
            }
            if (!default_eis.empty()) {
                lnx.eis_socket_path = default_eis;
                break;
            }
            std::cout << "EIS socket path is required.\n";
        }
        std::cout << "\n";
    }

    if (input == "UInput") {
        return input_uinput_width_height();
    }

    return true;
}

bool Interactor::input_uinput_width_height()
{
    auto& lnx = config_.configuration().lnx;

    auto read_dimension = [&](const std::string& label, int& value) -> bool {
        while (true) {
            auto buffer = read_line(std::format("Screen {} [{}]: ", label, value));
            if (!buffer) {
                input_aborted_ = true;
                return false;
            }

            if (buffer->empty() && value > 0) {
                return true;
            }

            int parsed = 0;
            const auto* first = buffer->data();
            const auto* last = first + buffer->size();
            const bool digits_only = std::ranges::all_of(*buffer, [](unsigned char c) { return c >= '0' && c <= '9'; });
            if (digits_only) {
                auto [ptr, ec] = std::from_chars(first, last, parsed);
                if (ec == std::errc { } && ptr == last && parsed > 0) {
                    value = parsed;
                    return true;
                }
            }

            std::cout << "Screen " << label << " must be a positive integer.\n";
        }
    };

    if (!read_dimension("width", lnx.uinput_screen_width) || !read_dimension("height", lnx.uinput_screen_height)) {
        return false;
    }

    std::cout << "\n";
    return true;
}

bool Interactor::select_wlroots()
{
    std::cout << "### Select Wayland Socket ###\n\n";

    std::cout << "\t1. Auto detect\n";
    std::cout << "\t2. Manual input\n";
    std::cout << "\n";

    auto selected = input(2);
    if (!selected) {
        input_aborted_ = true;
        return false;
    }

    switch (*selected) {
    case 1:
        return select_wlroots_auto_detect();

    case 2:
        return select_wlroots_manual_input();
    }

    return false;
}

bool Interactor::select_wlroots_auto_detect()
{
    std::cout << "Finding sockets...\n\n";

    auto list_handle = MaaToolkitDesktopWindowListCreate();
    OnScopeLeave([&]() { MaaToolkitDesktopWindowListDestroy(list_handle); });

    MaaToolkitDesktopWindowFindAll(list_handle);

    size_t size = MaaToolkitDesktopWindowListSize(list_handle);
    if (size == 0) {
        std::cout << "No sockets found!\n\n";
        return select_wlroots();
    }

    std::cout << "## Select Socket ##\n\n";

    for (size_t i = 0; i < size; ++i) {
        auto compositor = MaaToolkitDesktopWindowListAt(list_handle, i);

        auto id = MaaToolkitDesktopWindowGetHandle(compositor);
        std::string name = MaaToolkitDesktopWindowGetWindowName(compositor);
        std::string path = MaaToolkitDesktopWindowGetClassName(compositor);

        std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}\n\t\t{}\n\t\t{}\n", i + 1, id, name, path));
    }
    std::cout << "\n";

    auto selected = input(size);
    if (!selected) {
        input_aborted_ = true;
        return false;
    }
    const size_t index = static_cast<size_t>(*selected - 1);
    auto& wlr_config = config_.configuration().lnx;

    auto compositor = MaaToolkitDesktopWindowListAt(list_handle, index);

    wlr_config.wlr_socket_path = MaaToolkitDesktopWindowGetClassName(compositor);

    return true;
}

bool Interactor::select_wlroots_manual_input()
{
    auto socket_path = read_line("Please input Wayland socket path: ");
    if (!socket_path) {
        input_aborted_ = true;
        return false;
    }
    config_.configuration().lnx.wlr_socket_path = *socket_path;
    std::cout << "\n";

    return true;
}

bool Interactor::select_resource()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& all_resources = config_.interface_data().resource;
    const auto& current_controller = config_.configuration().controller.name;

    if (all_resources.empty()) {
        LogError << "Resource is empty";
        return false;
    }

    // Filter resources by current controller
    std::vector<const InterfaceData::Resource*> available_resources;
    for (const auto& res : all_resources) {
        // If controller list is empty, resource supports all controllers
        if (res.controller.empty() || std::ranges::find(res.controller, current_controller) != res.controller.end()) {
            available_resources.push_back(&res);
        }
    }

    if (available_resources.empty()) {
        LogError << "No resource available for controller" << VAR(current_controller);
        return false;
    }

    int index = 0;
    if (available_resources.size() != 1) {
        std::cout << "### Select resource ###\n\n";
        for (size_t i = 0; i < available_resources.size(); ++i) {
            const auto& res = *available_resources[i];
            std::string display_name = get_display_name(res.name, res.label);
            std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}\n", i + 1, display_name));
            if (!res.description.empty()) {
                std::string desc_text = read_text_content(res.description);
                std::cout << "\t   " << MAA_NS::utf8_to_crt(desc_text) << "\n";
            }
        }
        std::cout << "\n";
        auto selected = input(available_resources.size());
        if (!selected) {
            input_aborted_ = true;
            return false;
        }
        index = *selected - 1;
    }
    else {
        index = 0;
    }
    const auto& resource = *available_resources[index];

    config_.configuration().resource = resource.name;

    return true;
}

bool Interactor::add_task()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& all_data_tasks = config_.interface_data().task;
    const auto& current_resource = config_.configuration().resource;
    const auto& current_controller = config_.configuration().controller.name;

    if (all_data_tasks.empty()) {
        LogError << "Task is empty";
        std::cout << "No tasks available.\n\n";
        return mpause();
    }

    // Filter tasks by current resource and controller
    std::vector<const InterfaceData::Task*> available_tasks;
    for (const auto& task : all_data_tasks) {
        // If resource list is empty, task supports all resources
        if (!task.resource.empty() && std::ranges::find(task.resource, current_resource) == task.resource.end()) {
            continue;
        }
        // If controller list is empty, task supports all controllers
        if (!task.controller.empty() && std::ranges::find(task.controller, current_controller) == task.controller.end()) {
            continue;
        }
        available_tasks.push_back(&task);
    }

    if (available_tasks.empty()) {
        LogError << "No task available for resource" << VAR(current_resource) << "and controller" << VAR(current_controller);
        return false;
    }

    std::cout << "### Add task ###\n\n";
    for (size_t i = 0; i < available_tasks.size(); ++i) {
        const auto& task = *available_tasks[i];
        std::string display_name = get_display_name(task.name, task.label);
        std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}\n", i + 1, display_name));
        if (!task.description.empty()) {
            std::string desc_text = read_text_content(task.description);
            std::cout << "\t   " << MAA_NS::utf8_to_crt(desc_text) << "\n";
        }
    }
    std::cout << "\n";
    auto input_indexes = input_multi(available_tasks.size());
    if (!input_indexes) {
        input_aborted_ = true;
        return false;
    }

    std::vector<Configuration::Task> selected_tasks;
    for (int index : *input_indexes) {
        const auto& data_task = *available_tasks[index - 1];
        std::string task_display_name = get_display_name(data_task.name, data_task.label);

        std::vector<Configuration::Option> config_options;
        for (const auto& option_name : data_task.option) {
            if (!process_option(option_name, task_display_name, config_options)) {
                LogWarn << "Failed to process option" << VAR(data_task.name) << VAR(option_name);
                return false;
            }
        }

        selected_tasks.emplace_back(Configuration::Task { .name = data_task.name, .option = std::move(config_options) });
    }

    config_.configuration().task.insert(
        config_.configuration().task.end(),
        std::make_move_iterator(selected_tasks.begin()),
        std::make_move_iterator(selected_tasks.end()));

    return true;
}

bool Interactor::add_default_tasks()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& all_data_tasks = config_.interface_data().task;
    const auto& current_resource = config_.configuration().resource;
    const auto& current_controller = config_.configuration().controller.name;

    for (const auto& task : all_data_tasks) {
        // Skip tasks without default_check
        if (!task.default_check) {
            continue;
        }

        // Check if task supports current resource
        if (!task.resource.empty() && std::ranges::find(task.resource, current_resource) == task.resource.end()) {
            continue;
        }

        // Check if task supports current controller
        if (!task.controller.empty() && std::ranges::find(task.controller, current_controller) == task.controller.end()) {
            continue;
        }

        std::string task_display_name = get_display_name(task.name, task.label);

        // Process options for this task
        std::vector<Configuration::Option> config_options;
        for (const auto& option_name : task.option) {
            // 自动添加 default_check 任务：带 default_case 的 option 直接采用默认值，不打断批量添加
            if (!process_option(option_name, task_display_name, config_options, /*auto_accept_default=*/true)) {
                LogWarn << "Failed to process option for default task" << VAR(task.name) << VAR(option_name);
                return false;
            }
        }

        config_.configuration().task.emplace_back(Configuration::Task { .name = task.name, .option = std::move(config_options) });
        std::cout << "Auto-added default task: " << MAA_NS::utf8_to_crt(task_display_name) << "\n";
    }

    if (!config_.configuration().task.empty()) {
        std::cout << "\n";
    }

    return true;
}

bool Interactor::process_option(
    const std::string& option_name,
    const std::string& task_display_name,
    std::vector<MAA_PROJECT_INTERFACE_NS::Configuration::Option>& config_options,
    bool auto_accept_default)
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    if (!config_.interface_data().option.contains(option_name)) {
        LogError << "Option not found" << VAR(option_name);
        return false;
    }

    const auto& opt = config_.interface_data().option.at(option_name);

    // v2.3.1: check option applicability
    if (!opt.controller.empty() && std::ranges::find(opt.controller, config_.configuration().controller.name) == opt.controller.end()) {
        return true;
    }
    if (!opt.resource.empty() && std::ranges::find(opt.resource, config_.configuration().resource) == opt.resource.end()) {
        return true;
    }

    Configuration::Option config_opt;
    config_opt.name = option_name;

    std::string opt_display_name = get_display_name(option_name, opt.label);

    std::vector<const InterfaceData::Option::Case*> selected_cases;

    // 协议：default_case 是 option 的「初始选中值」，不是「跳过交互」。这里只把它解析成预选 case，
    // 交互流程中仍会列出 cases 让用户确认（回车即采用默认值）；仅自动补全流程直接采用。
    const InterfaceData::Option::Case* default_case_ptr = nullptr;
    if (auto* str = std::get_if<std::string>(&opt.default_case); str && !str->empty()) {
        auto case_iter = std::ranges::find(opt.cases, *str, std::mem_fn(&InterfaceData::Option::Case::name));
        if (case_iter != opt.cases.end()) {
            default_case_ptr = &(*case_iter);
        }
        else if (opt.type == InterfaceData::Option::Type::Select || opt.type == InterfaceData::Option::Type::Switch) {
            // 旧实现会把这个不存在的名字直接写进配置，随后被 check_task 判定失效而删掉整个任务
            LogWarn << "default_case not found in cases, ignoring it" << VAR(option_name) << VAR(*str);
        }
    }

    switch (opt.type) {
    case InterfaceData::Option::Type::Select: {
        if (opt.cases.empty()) {
            LogError << "Select option must have at least 1 case" << VAR(option_name);
            return false;
        }

        if (default_case_ptr && auto_accept_default) {
            config_opt.value = default_case_ptr->name;
            selected_cases.push_back(default_case_ptr);
            break;
        }

        std::cout << MAA_NS::utf8_to_crt(std::format("\n\n## Select option \"{}\" for \"{}\" ##\n\n", opt_display_name, task_display_name));
        if (!opt.description.empty()) {
            std::string desc_text = read_text_content(opt.description);
            std::cout << MAA_NS::utf8_to_crt(desc_text) << "\n\n";
        }

        size_t default_index = 0; // 1-based, 0 means no default
        for (size_t i = 0; i < opt.cases.size(); ++i) {
            const auto& case_item = opt.cases[i];
            std::string case_display_name = get_display_name(case_item.name, case_item.label);
            const bool is_default = default_case_ptr == &case_item;
            if (is_default) {
                default_index = i + 1;
            }
            std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}{}\n", i + 1, case_display_name, is_default ? " (default)" : ""));
            if (!case_item.description.empty()) {
                std::string case_desc = read_text_content(case_item.description);
                std::cout << "\t   " << MAA_NS::utf8_to_crt(case_desc) << "\n";
            }
        }
        std::cout << "\n";

        const auto selected = input(opt.cases.size(), "Please input", static_cast<int>(default_index));
        if (!selected) {
            input_aborted_ = true;
            return false;
        }
        if (*selected < 1 || static_cast<size_t>(*selected) > opt.cases.size()) {
            LogError << "Invalid selection" << VAR(option_name) << VAR(*selected);
            return false;
        }

        const size_t case_index = static_cast<size_t>(*selected) - 1;
        config_opt.value = opt.cases[case_index].name;
        selected_cases.push_back(&opt.cases[case_index]);
    } break;

    case InterfaceData::Option::Type::Switch: {
        // Switch 类型必须有恰好两个 cases
        if (opt.cases.size() < 2) {
            LogError << "Switch option must have at least 2 cases" << VAR(option_name) << VAR(opt.cases.size());
            return false;
        }

        static const std::unordered_set<std::string> yes_names = { "Yes", "yes", "Y", "y" };
        static const std::unordered_set<std::string> no_names = { "No", "no", "N", "n" };

        auto find_named_case = [&](const std::unordered_set<std::string>& names) -> const InterfaceData::Option::Case* {
            for (const auto& case_item : opt.cases) {
                if (names.contains(case_item.name)) {
                    return &case_item;
                }
            }
            return nullptr;
        };
        const auto* yes_case = find_named_case(yes_names);
        const auto* no_case = find_named_case(no_names);

        if (default_case_ptr && auto_accept_default) {
            config_opt.value = default_case_ptr->name;
            selected_cases.push_back(default_case_ptr);
            break;
        }

        std::cout << MAA_NS::utf8_to_crt(std::format("\n\n## Switch option \"{}\" for \"{}\" ##\n\n", opt_display_name, task_display_name));
        if (!opt.description.empty()) {
            std::string desc_text = read_text_content(opt.description);
            std::cout << MAA_NS::utf8_to_crt(desc_text) << "\n\n";
        }

        // 协议要求 switch 的 case.name 使用 Yes/No 系列。命名不匹配时退回编号选择：
        // 旧实现的 fallback 会把 Y 与 N 都映射到同一个 case（常见于按 cases[0] 兜底）
        if (opt.cases.size() != 2 || !yes_case || !no_case) {
            LogWarn << "Switch option does not contain exactly two Yes/No cases, fall back to numbered selection" << VAR(option_name);

            size_t fallback_default_index = 0;
            for (size_t i = 0; i < opt.cases.size(); ++i) {
                const auto& case_item = opt.cases[i];
                std::string case_display_name = get_display_name(case_item.name, case_item.label);
                const bool is_default = default_case_ptr == &case_item;
                if (is_default) {
                    fallback_default_index = i + 1;
                }
                std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}{}\n", i + 1, case_display_name, is_default ? " (default)" : ""));
                if (!case_item.description.empty()) {
                    std::string case_desc = read_text_content(case_item.description);
                    std::cout << "\t   " << MAA_NS::utf8_to_crt(case_desc) << "\n";
                }
            }
            std::cout << "\n";

            const auto selected = input(opt.cases.size(), "Please input", static_cast<int>(fallback_default_index));
            if (!selected) {
                input_aborted_ = true;
                return false;
            }
            if (*selected < 1 || static_cast<size_t>(*selected) > opt.cases.size()) {
                LogError << "Invalid selection" << VAR(option_name) << VAR(*selected);
                return false;
            }

            const auto* fallback_case = &opt.cases[static_cast<size_t>(*selected) - 1];
            selected_cases.push_back(fallback_case);
            config_opt.value = fallback_case->name;
            std::cout << "\n";
            break;
        }

        const std::string yes_display_name = get_display_name(yes_case->name, yes_case->label);
        const std::string no_display_name = get_display_name(no_case->name, no_case->label);
        std::cout << "\tY. " << MAA_NS::utf8_to_crt(yes_display_name) << "\n";
        std::cout << "\tN. " << MAA_NS::utf8_to_crt(no_display_name) << "\n";
        std::cout << "\n";

        // default_case 只在能被 Y/N 表达时作为预选值（指向第三个 case 时忽略）
        const auto* default_yn_case = (default_case_ptr == yes_case || default_case_ptr == no_case) ? default_case_ptr : nullptr;
        if (default_yn_case) {
            std::cout << std::format("Input Y/N (default {}): ", default_yn_case == yes_case ? "Y" : "N");
        }
        else {
            std::cout << "Input Y/N: ";
        }

        std::string buffer;
        bool is_yes = false;
        while (true) {
            auto input_buffer = read_line();
            if (!input_buffer) {
                input_aborted_ = true;
                return false;
            }
            buffer = std::move(*input_buffer);

            if (buffer.empty() && default_yn_case) {
                is_yes = default_yn_case == yes_case;
                break;
            }
            else if (yes_names.contains(buffer)) {
                is_yes = true;
                break;
            }
            else if (no_names.contains(buffer)) {
                is_yes = false;
                break;
            }
            else {
                std::cout << "Invalid input, please input Y/N: ";
            }
        }

        const auto* matched_case = is_yes ? yes_case : no_case;
        selected_cases.push_back(matched_case);
        config_opt.value = matched_case->name;
        std::cout << "\n";
    } break;

    case InterfaceData::Option::Type::Checkbox: {
        if (opt.cases.empty()) {
            LogError << "Checkbox option must have at least 1 case" << VAR(option_name);
            return false;
        }

        auto selection_count_valid = [&opt](const std::vector<std::string>& values) {
            return (!opt.min_count || values.size() >= *opt.min_count) && (!opt.max_count || values.size() <= *opt.max_count);
        };

        // 与 select/switch 一致：default_case 只作为「初始选中值」解析成预选编号（1-based），
        // 交互流程中列出 cases（预选标 [x]，回车即保持预选）；仅自动补全流程直接采用。
        std::vector<int> default_indexes;
        if (auto* vec = std::get_if<std::vector<std::string>>(&opt.default_case); vec && !vec->empty()) {
            for (const auto& default_name : *vec) {
                auto case_iter = std::ranges::find(opt.cases, default_name, std::mem_fn(&InterfaceData::Option::Case::name));
                if (case_iter == opt.cases.end()) {
                    // 旧实现会把不存在的名字直接写进配置，随后被 check_task 判定失效而删掉整个任务
                    LogWarn << "default_case not found in cases, ignoring it" << VAR(option_name) << VAR(default_name);
                    continue;
                }
                // 记录 1-based 预选编号，按 cases 顺序排序，与 Configurator 的应用顺序保持一致
                default_indexes.emplace_back(static_cast<int>(case_iter - opt.cases.begin()) + 1);
            }
            std::ranges::sort(default_indexes);
            default_indexes.erase(std::unique(default_indexes.begin(), default_indexes.end()), default_indexes.end());
        }

        if (opt.max_count == 0) {
            config_opt.values.clear();
        }
        else if (!default_indexes.empty() && auto_accept_default) {
            for (const int index : default_indexes) {
                config_opt.values.emplace_back(opt.cases[static_cast<size_t>(index) - 1].name);
            }
            if (!selection_count_valid(config_opt.values)) {
                LogError << "Default checkbox selection count is invalid" << VAR(option_name);
                return false;
            }
        }
        else {
            std::cout << MAA_NS::utf8_to_crt(
                std::format("\n\n## Checkbox option \"{}\" for \"{}\" (multi-select) ##\n\n", opt_display_name, task_display_name));
            if (!opt.description.empty()) {
                std::string desc_text = read_text_content(opt.description);
                std::cout << MAA_NS::utf8_to_crt(desc_text) << "\n\n";
            }
            for (size_t i = 0; i < opt.cases.size(); ++i) {
                const auto& case_item = opt.cases[i];
                std::string case_display_name = get_display_name(case_item.name, case_item.label);
                const bool is_default = std::ranges::find(default_indexes, static_cast<int>(i + 1)) != default_indexes.end();
                std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. [{}] {}\n", i + 1, is_default ? "x" : " ", case_display_name));
                if (!case_item.description.empty()) {
                    std::string case_desc = read_text_content(case_item.description);
                    std::cout << "\t   " << MAA_NS::utf8_to_crt(case_desc) << "\n";
                }
            }
            if (!default_indexes.empty()) {
                std::cout << MAA_NS::utf8_to_crt("\t(empty input keeps the default selection)\n");
            }
            std::string constraint_text;
            if (opt.min_count) {
                constraint_text += std::format("at least {}", *opt.min_count);
            }
            if (opt.max_count) {
                if (!constraint_text.empty()) {
                    constraint_text += ", ";
                }
                constraint_text += std::format("at most {}", *opt.max_count);
            }
            if (!constraint_text.empty()) {
                std::cout << MAA_NS::utf8_to_crt(std::format("\tSelect {} item(s)\n", constraint_text));
            }
            std::cout << "\n";

            while (true) {
                auto indexes = input_multi(opt.cases.size(), "Please input multiple", default_indexes);
                if (!indexes) {
                    input_aborted_ = true;
                    return false;
                }

                config_opt.values.clear();
                std::unordered_set<int> selected_indexes;
                for (int idx : *indexes) {
                    if (idx < 1 || static_cast<size_t>(idx) > opt.cases.size()) {
                        LogError << "Invalid selection" << VAR(option_name) << VAR(idx);
                        return false;
                    }
                    if (!selected_indexes.insert(idx).second) {
                        continue;
                    }
                    config_opt.values.emplace_back(opt.cases[static_cast<size_t>(idx) - 1].name);
                }

                if (selection_count_valid(config_opt.values)) {
                    break;
                }
                std::cout << MAA_NS::utf8_to_crt("Invalid selection count, please retry.\n");
            }
        }

        for (const auto& val : config_opt.values) {
            auto it = std::ranges::find(opt.cases, val, std::mem_fn(&InterfaceData::Option::Case::name));
            if (it != opt.cases.end()) {
                selected_cases.push_back(&(*it));
            }
        }
    } break;

    case InterfaceData::Option::Type::Input: {
        std::cout << MAA_NS::utf8_to_crt(std::format("\n\n## Input option \"{}\" for \"{}\" ##\n\n", opt_display_name, task_display_name));
        if (!opt.description.empty()) {
            std::string desc_text = read_text_content(opt.description);
            std::cout << MAA_NS::utf8_to_crt(desc_text) << "\n\n";
        }

        for (const auto& input_def : opt.inputs) {
            std::string default_val = input_def.default_;
            std::string input_display_name = get_display_name(input_def.name, input_def.label);
            if (!input_def.description.empty()) {
                std::string input_desc = read_text_content(input_def.description);
                std::cout << MAA_NS::utf8_to_crt(input_desc) << "\n";
            }
            std::cout << MAA_NS::utf8_to_crt(std::format("{} [{}]: ", input_display_name, input_def.password ? "********" : default_val));

            auto read_input_value = [&]() -> std::optional<std::string> {
                if (input_def.password) {
                    auto hidden_input = read_hidden_line();
                    if (!hidden_input) {
                        input_aborted_ = true;
                        return std::nullopt;
                    }
                    return hidden_input;
                }

                auto plain_input = read_line();
                if (!plain_input) {
                    input_aborted_ = true;
                    return std::nullopt;
                }
                return plain_input;
            };

            auto buffer = read_input_value();
            if (!buffer) {
                return false;
            }

            std::string value = buffer->empty() ? default_val : *buffer;

            if (!input_def.verify.empty()) {
                if (auto pattern = MAA_NS::regex_valid(MAA_NS::to_u16(input_def.verify))) {
                    auto value_u16 = MAA_NS::to_u16(value);
                    while (!boost::regex_match(value_u16, *pattern)) {
                        std::string error_msg =
                            input_def.pattern_msg.empty() ? "Invalid input, please retry: " : input_def.pattern_msg + ": ";
                        std::cout << MAA_NS::utf8_to_crt(error_msg);
                        buffer = read_input_value();
                        if (!buffer) {
                            return false;
                        }
                        value = buffer->empty() ? default_val : *buffer;
                        value_u16 = MAA_NS::to_u16(value);
                    }
                }
            }

            config_opt.inputs[input_def.name] = value;
        }
        std::cout << "\n";
    } break;
    }

    std::vector<Configuration::Option> nested_options;
    for (const auto* sc : selected_cases) {
        for (const auto& sub_option_name : sc->option) {
            if (!process_option(sub_option_name, task_display_name, nested_options, auto_accept_default)) {
                return false;
            }
        }
    }

    config_options.emplace_back(std::move(config_opt));
    config_options.insert(
        config_options.end(),
        std::make_move_iterator(nested_options.begin()),
        std::make_move_iterator(nested_options.end()));

    return true;
}

bool Interactor::delete_task()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    auto& all_config_tasks = config_.configuration().task;
    if (all_config_tasks.empty()) {
        LogError << "Task is empty";
        return false;
    }

    std::cout << "### Delete task ###\n\n";

    print_config_tasks();

    auto input_indexes = input_multi(all_config_tasks.size());
    if (!input_indexes) {
        input_aborted_ = true;
        return false;
    }

    std::unordered_set<size_t> indexes;
    for (int index : *input_indexes) {
        indexes.insert(static_cast<size_t>(index - 1));
    }

    std::vector<Configuration::Task> remaining_tasks;
    remaining_tasks.reserve(all_config_tasks.size());
    for (size_t i = 0; i < all_config_tasks.size(); ++i) {
        if (!indexes.contains(i)) {
            remaining_tasks.emplace_back(std::move(all_config_tasks[i]));
        }
    }
    all_config_tasks = std::move(remaining_tasks);

    return true;
}

bool Interactor::move_task()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    auto& all_config_tasks = config_.configuration().task;
    if (all_config_tasks.empty()) {
        LogError << "Task is empty";
        return false;
    }

    std::cout << "### Move task ###\n\n";

    print_config_tasks(true);

    auto from = input(all_config_tasks.size(), "From");
    if (!from) {
        input_aborted_ = true;
        return false;
    }
    auto to = input(all_config_tasks.size(), "To");
    if (!to) {
        input_aborted_ = true;
        return false;
    }

    const size_t from_index = static_cast<size_t>(*from - 1);
    const size_t to_index = static_cast<size_t>(*to - 1);
    auto task = std::move(all_config_tasks[from_index]);
    all_config_tasks.erase(all_config_tasks.begin() + from_index);
    all_config_tasks.insert(all_config_tasks.begin() + to_index, std::move(task));

    return true;
}

bool Interactor::process_level_options(
    const std::vector<std::string>& option_names,
    std::vector<MAA_PROJECT_INTERFACE_NS::Configuration::Option>& config_options,
    const std::string& level_label)
{
    if (option_names.empty()) {
        return true;
    }

    std::vector<MAA_PROJECT_INTERFACE_NS::Configuration::Option> new_options;
    for (const auto& option_name : option_names) {
        if (!process_option(option_name, level_label, new_options)) {
            LogWarn << "Failed to process" << level_label << "option" << VAR(option_name);
            return false;
        }
    }

    config_options = std::move(new_options);
    return true;
}

void Interactor::edit_task()
{
    // TODO
}

void Interactor::print_config_tasks(bool with_index) const
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    auto& all_config_tasks = config_.configuration().task;
    const auto& groups = config_.interface_data().group;

    auto print_task_options = [&](const Configuration::Task& task) {
        for (const auto& opt : task.option) {
            if (!opt.value.empty()) {
                std::cout << "\t\t- " << MAA_NS::utf8_to_crt(opt.name) << ": " << MAA_NS::utf8_to_crt(opt.value) << "\n";
            }
            else if (!opt.values.empty()) {
                std::cout << "\t\t- " << MAA_NS::utf8_to_crt(opt.name) << ": [";
                for (size_t j = 0; j < opt.values.size(); ++j) {
                    if (j > 0) {
                        std::cout << ", ";
                    }
                    std::cout << MAA_NS::utf8_to_crt(opt.values[j]);
                }
                std::cout << "]\n";
            }
            else if (!opt.inputs.empty()) {
                std::cout << "\t\t- " << MAA_NS::utf8_to_crt(opt.name) << ":\n";
                for (const auto& [key, val] : opt.inputs) {
                    std::cout << "\t\t\t" << MAA_NS::utf8_to_crt(key) << ": "
                              << MAA_NS::utf8_to_crt(display_input_value(opt.name, key, val)) << "\n";
                }
            }
        }
    };

    // v2.4.0: display tasks grouped if groups are defined
    if (!groups.empty() && !with_index) {
        std::unordered_set<std::string> printed_tasks;

        for (const auto& grp : groups) {
            std::string grp_display = get_display_name(grp.name, grp.label);
            bool has_task_in_group = false;
            for (const auto& cfg_task : all_config_tasks) {
                auto data_it = std::ranges::find(config_.interface_data().task, cfg_task.name, std::mem_fn(&InterfaceData::Task::name));
                if (data_it == config_.interface_data().task.end()) {
                    continue;
                }
                if (std::ranges::find(data_it->group, grp.name) == data_it->group.end()) {
                    continue;
                }
                has_task_in_group = true;
                break;
            }
            if (!has_task_in_group) {
                continue;
            }

            std::cout << "  [" << MAA_NS::utf8_to_crt(grp_display) << "]\n";
            for (const auto& cfg_task : all_config_tasks) {
                auto data_it = std::ranges::find(config_.interface_data().task, cfg_task.name, std::mem_fn(&InterfaceData::Task::name));
                if (data_it == config_.interface_data().task.end()) {
                    continue;
                }
                if (std::ranges::find(data_it->group, grp.name) == data_it->group.end()) {
                    continue;
                }
                std::cout << MAA_NS::utf8_to_crt(std::format("\t- {}\n", cfg_task.name));
                print_task_options(cfg_task);
                printed_tasks.insert(cfg_task.name);
            }
        }

        bool has_ungrouped = false;
        for (const auto& cfg_task : all_config_tasks) {
            if (printed_tasks.contains(cfg_task.name)) {
                continue;
            }
            if (!has_ungrouped) {
                std::cout << "  [Other]\n";
                has_ungrouped = true;
            }
            std::cout << MAA_NS::utf8_to_crt(std::format("\t- {}\n", cfg_task.name));
            print_task_options(cfg_task);
        }
    }
    else {
        for (size_t i = 0; i < all_config_tasks.size(); ++i) {
            const auto& task = all_config_tasks[i];
            if (with_index) {
                std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}\n", i + 1, task.name));
            }
            else {
                std::cout << MAA_NS::utf8_to_crt(std::format("\t- {}\n", task.name));
            }
            print_task_options(task);
        }
    }
    std::cout << "\n";
}

bool Interactor::check_validity()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    if (config_.configuration().controller.type == InterfaceData::Controller::Type::Win32
        && config_.configuration().win32.hwnd == nullptr) {
        auto& name = config_.configuration().controller.name;
        auto controller_iter = std::ranges::find(config_.interface_data().controller, name, std::mem_fn(&InterfaceData::Controller::name));

        if (controller_iter == config_.interface_data().controller.end()) {
            LogError << "Contorller not found" << VAR(name);
            return false;
        }

        return select_win32_hwnd(controller_iter->win32);
    }

    if (config_.configuration().controller.type == InterfaceData::Controller::Type::MacOS) {
        auto& mac = config_.configuration().macos;
        if (mac.window_id == 0 || mac.title.empty()) {
            auto& name = config_.configuration().controller.name;
            auto controller_iter =
                std::ranges::find(config_.interface_data().controller, name, std::mem_fn(&InterfaceData::Controller::name));

            if (controller_iter == config_.interface_data().controller.end()) {
                LogError << "Controller not found" << VAR(name);
                return false;
            }

            select_macos(controller_iter->macos);
            return mac.window_id != 0;
        }
    }

    if (config_.configuration().controller.type == InterfaceData::Controller::Type::Linux) {
        if (!kLinuxSupported) {
            LogError << "Linux controller is only available on Linux";
            return false;
        }

        const auto* controller = find_current_controller();
        if (!controller) {
            LogError << "Controller not found" << VAR(config_.configuration().controller.name);
            return false;
        }

        const auto& lnx = config_.configuration().lnx;
        const auto& controller_lnx = controller->lnx;
        const std::string screencap = normalize_linux_screencap_method(controller_lnx.screencap);
        const std::string input = normalize_linux_input_method(controller_lnx.input);
        const bool needs_wlr_socket = lnx.wlr_socket_path.empty() && (screencap == "Wlr" || input == "Wlr");
        const bool needs_eis_socket = lnx.eis_socket_path.empty() && input == "Libei";
        const bool needs_uinput_dimensions = input == "UInput" && (lnx.uinput_screen_width <= 0 || lnx.uinput_screen_height <= 0);
        if (needs_wlr_socket || needs_eis_socket || needs_uinput_dimensions) {
            if (!select_linux(controller_lnx)) {
                return false;
            }

            const bool wlr_socket_missing = needs_wlr_socket && lnx.wlr_socket_path.empty();
            const bool eis_socket_missing = needs_eis_socket && lnx.eis_socket_path.empty();
            const bool uinput_dimensions_missing =
                needs_uinput_dimensions && (lnx.uinput_screen_width <= 0 || lnx.uinput_screen_height <= 0);
            if (wlr_socket_missing || eis_socket_missing || uinput_dimensions_missing) {
                LogError << "Required Linux controller value is missing";
                return false;
            }
        }
    }

    if (config_.configuration().controller.type == InterfaceData::Controller::Type::PlayCover) {
        if (!kPlayCoverSupported) {
            LogError << "PlayCover controller is only available on macOS";
            return false;
        }

        auto& pc = config_.configuration().playcover;
        if (pc.address.empty()) {
            auto& name = config_.configuration().controller.name;
            auto controller_iter =
                std::ranges::find(config_.interface_data().controller, name, std::mem_fn(&InterfaceData::Controller::name));

            if (controller_iter != config_.interface_data().controller.end()) {
                select_playcover(controller_iter->playcover);
            }
        }

        if (pc.address.empty()) {
            LogError << "PlayCover address is empty";
            return false;
        }
    }

    if (config_.configuration().controller.type == InterfaceData::Controller::Type::Gamepad) {
        if (!kGamepadSupported) {
            LogError << "Gamepad controller is only available on Windows";
            return false;
        }

        // Gamepad hwnd is optional (for screencap), so no validation needed
        // But we need to select gamepad type if not configured
        if (config_.configuration().gamepad.gamepad_type.empty()) {
            auto& name = config_.configuration().controller.name;
            auto controller_iter =
                std::ranges::find(config_.interface_data().controller, name, std::mem_fn(&InterfaceData::Controller::name));

            if (controller_iter != config_.interface_data().controller.end()) {
                select_gamepad(controller_iter->gamepad);
            }
        }
    }

    return true;
}

bool Interactor::ensure_runtime_options()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    auto& config = config_.configuration();
    auto has_option = [](const std::vector<Configuration::Option>& options, const std::string& name) {
        return std::ranges::any_of(options, [&](const auto& option) { return option.name == name; });
    };

    for (const auto& option_name : config_.interface_data().global_option) {
        if (!has_option(config.global_option, option_name)
            && !process_option(option_name, "Global", config.global_option, /*auto_accept_default=*/true)) {
            return false;
        }
    }

    const auto resource_iter =
        std::ranges::find(config_.interface_data().resource, config.resource, std::mem_fn(&InterfaceData::Resource::name));
    if (resource_iter != config_.interface_data().resource.end()) {
        for (const auto& option_name : resource_iter->option) {
            if (!has_option(config.resource_option, option_name)
                && !process_option(option_name, "Resource", config.resource_option, /*auto_accept_default=*/true)) {
                return false;
            }
        }
    }

    const auto controller_iter =
        std::ranges::find(config_.interface_data().controller, config.controller.name, std::mem_fn(&InterfaceData::Controller::name));
    if (controller_iter != config_.interface_data().controller.end()) {
        for (const auto& option_name : controller_iter->option) {
            if (!has_option(config.controller_option, option_name)
                && !process_option(option_name, "Controller", config.controller_option, /*auto_accept_default=*/true)) {
                return false;
            }
        }
    }

    return true;
}

bool Interactor::ensure_pretask_options()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& current_resource = config_.configuration().resource;
    const auto& current_controller = config_.configuration().controller.name;

    for (const auto& data_pretask : Parser::flatten_pretask(config_.interface_data().pretask)) {
        if (!data_pretask.resource.empty() && std::ranges::find(data_pretask.resource, current_resource) == data_pretask.resource.end()) {
            continue;
        }
        if (!data_pretask.controller.empty()
            && std::ranges::find(data_pretask.controller, current_controller) == data_pretask.controller.end()) {
            continue;
        }
        if (data_pretask.option.empty()) {
            continue;
        }

        const std::string identifier = data_pretask.name.empty() ? data_pretask.exec : data_pretask.name;
        const std::string display_name = get_display_name(identifier, data_pretask.label);
        auto& config_pretasks = config_.configuration().pretask;
        auto config_pretask_iter =
            std::ranges::find_if(config_pretasks, [&](const auto& config_pretask) { return config_pretask.name == identifier; });

        if (config_pretask_iter == config_pretasks.end()) {
            Configuration::Pretask config_pretask;
            config_pretask.name = identifier;
            for (const auto& option_name : data_pretask.option) {
                if (!process_option(option_name, display_name, config_pretask.option, /*auto_accept_default=*/true)) {
                    return false;
                }
            }
            config_pretasks.emplace_back(std::move(config_pretask));
            continue;
        }

        for (const auto& option_name : data_pretask.option) {
            if (!ensure_pretask_option_tree(option_name, display_name, *config_pretask_iter)) {
                return false;
            }
        }
    }

    return true;
}

bool Interactor::ensure_pretask_option_tree(
    const std::string& option_name,
    const std::string& pretask_display_name,
    MAA_PROJECT_INTERFACE_NS::Configuration::Pretask& config_pretask)
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    auto config_option_iter =
        std::ranges::find_if(config_pretask.option, [&](const auto& config_option) { return config_option.name == option_name; });
    if (config_option_iter == config_pretask.option.end()) {
        // pretask 选项树补全可能在 -d 直跑时触发，不阻塞：带 default_case 的 option 直接采用默认值
        return process_option(option_name, pretask_display_name, config_pretask.option, /*auto_accept_default=*/true);
    }

    auto data_option_iter = config_.interface_data().option.find(option_name);
    if (data_option_iter == config_.interface_data().option.end()) {
        LogError << "Option not found" << VAR(option_name);
        return false;
    }

    const auto& data_option = data_option_iter->second;
    if (!data_option.controller.empty()
        && std::ranges::find(data_option.controller, config_.configuration().controller.name) == data_option.controller.end()) {
        return true;
    }
    if (!data_option.resource.empty()
        && std::ranges::find(data_option.resource, config_.configuration().resource) == data_option.resource.end()) {
        return true;
    }

    std::vector<std::vector<std::string>> active_option_groups;
    switch (data_option.type) {
    case InterfaceData::Option::Type::Select:
    case InterfaceData::Option::Type::Switch: {
        auto case_iter =
            std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == config_option_iter->value; });
        if (case_iter == data_option.cases.end()) {
            LogError << "Option case not found" << VAR(option_name) << VAR(config_option_iter->value);
            return false;
        }
        active_option_groups.emplace_back(case_iter->option);
    } break;

    case InterfaceData::Option::Type::Checkbox: {
        for (const auto& value : config_option_iter->values) {
            auto case_iter = std::ranges::find_if(data_option.cases, [&](const auto& data_case) { return data_case.name == value; });
            if (case_iter == data_option.cases.end()) {
                LogError << "Option case not found" << VAR(option_name) << VAR(value);
                return false;
            }
            active_option_groups.emplace_back(case_iter->option);
        }
    } break;

    case InterfaceData::Option::Type::Input:
        break;
    }

    for (const auto& active_options : active_option_groups) {
        for (const auto& active_option : active_options) {
            if (!ensure_pretask_option_tree(active_option, pretask_display_name, config_pretask)) {
                return false;
            }
        }
    }

    return true;
}

bool Interactor::save_config()
{
    if (config_.save(user_path_)) {
        return true;
    }

    LogError << "Failed to save configuration" << VAR(user_path_);
    std::cout << "\nFailed to save configuration.\n\n";
    return false;
}

bool Interactor::mpause()
{
    auto line = read_line("\nPress Enter to continue...");
    if (!line) {
        input_aborted_ = true;
        return false;
    }
    return true;
}

bool Interactor::apply_preset()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& presets = config_.interface_data().preset;
    if (presets.empty()) {
        std::cout << "No presets available.\n\n";
        return false;
    }

    std::cout << "### Apply preset ###\n\n";
    for (size_t i = 0; i < presets.size(); ++i) {
        const auto& p = presets[i];
        std::string display_name = get_display_name(p.name, p.label);
        std::cout << MAA_NS::utf8_to_crt(std::format("\t{}. {}\n", i + 1, display_name));
        if (!p.description.empty()) {
            std::string desc_text = read_text_content(p.description);
            std::cout << "\t   " << MAA_NS::utf8_to_crt(desc_text) << "\n";
        }
    }
    std::cout << "\n";

    auto selected = input(presets.size());
    if (!selected) {
        input_aborted_ = true;
        return false;
    }
    const auto& preset = presets.at(static_cast<size_t>(*selected - 1));

    std::vector<Configuration::Task> preset_tasks;
    for (const auto& preset_task : preset.task) {
        if (!preset_task.enabled) {
            continue;
        }

        auto data_iter = std::ranges::find(config_.interface_data().task, preset_task.name, std::mem_fn(&InterfaceData::Task::name));
        if (data_iter == config_.interface_data().task.end()) {
            LogWarn << "Preset references unknown task" << VAR(preset_task.name);
            continue;
        }

        Configuration::Task config_task;
        config_task.name = preset_task.name;

        for (const auto& [opt_name, opt_value] : preset_task.option) {
            auto opt_iter = config_.interface_data().option.find(opt_name);
            if (opt_iter == config_.interface_data().option.end()) {
                continue;
            }

            Configuration::Option config_opt;
            config_opt.name = opt_name;

            switch (opt_iter->second.type) {
            case InterfaceData::Option::Type::Select:
            case InterfaceData::Option::Type::Switch:
                if (opt_value.is_string()) {
                    config_opt.value = opt_value.as_string();
                }
                break;
            case InterfaceData::Option::Type::Checkbox:
                if (opt_value.is_array()) {
                    for (const auto& v : opt_value.as_array()) {
                        if (v.is_string()) {
                            config_opt.values.emplace_back(v.as_string());
                        }
                    }
                }
                break;
            case InterfaceData::Option::Type::Input:
                if (opt_value.is_object()) {
                    for (const auto& [k, v] : opt_value.as_object()) {
                        if (v.is_string()) {
                            config_opt.inputs[k] = v.as_string();
                        }
                    }
                }
                break;
            }

            config_task.option.emplace_back(std::move(config_opt));
        }

        preset_tasks.emplace_back(std::move(config_task));
    }

    config_.configuration().task = std::move(preset_tasks);
    std::string preset_display = get_display_name(preset.name, preset.label);
    std::cout << "Applied preset: " << MAA_NS::utf8_to_crt(preset_display) << "\n\n";

    return true;
}

std::string Interactor::get_display_name(const std::string& name, const std::string& label) const
{
    if (label.empty()) {
        return name;
    }
    // 翻译 label（如果以 $ 开头会被翻译）
    return config_.translate(label);
}

std::string Interactor::display_input_value(const std::string& option_name, const std::string& input_name, const std::string& value) const
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto& options = config_.interface_data().option;
    auto option_iter = options.find(option_name);
    if (option_iter == options.end()) {
        return value;
    }

    const auto is_password =
        std::ranges::any_of(option_iter->second.inputs, [&](const auto& input) { return input.name == input_name && input.password; });
    return is_password ? "********" : value;
}

std::string Interactor::read_text_content(const std::string& text) const
{
    if (text.empty()) {
        return { };
    }

    // 先翻译文本（如果以 $ 开头）
    std::string translated = config_.translate(text);

    // 尝试作为文件路径读取
    auto file_path = config_.resource_dir() / MAA_NS::path(translated);
    constexpr size_t kMaxPath = 255;
    if (MAA_NS::path_to_utf8_string(file_path).size() < kMaxPath && std::filesystem::exists(file_path)) {
        std::ifstream ifs(file_path);
        if (ifs.is_open()) {
            std::stringstream buffer;
            buffer << ifs.rdbuf();
            return buffer.str();
        }
    }

    // 不是文件，直接返回翻译后的文本
    return translated;
}
