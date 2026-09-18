#pragma once

#include "ProjectInterface/Configurator.h"

class Interactor
{
public:
    explicit Interactor(std::filesystem::path user_path);

    bool load(const std::filesystem::path& resource_path);
    void print_config() const;
    bool interact();
    bool run();

private:
    enum class ActionStatus
    {
        Incomplete,
        Complete,
        Exit,
        Aborted,
    };

    bool interact_for_first_time_use();

    void welcome() const;
    ActionStatus interact_once();
    ActionStatus action_status(bool completed) const;

    bool select_controller();
    bool select_adb();
    bool select_adb_auto_detect();
    bool select_adb_manual_input();

    bool select_win32_hwnd(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::Win32Config& win32_config);
    bool select_macos(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::MacOSConfig& macos_config);
    bool select_playcover(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::PlayCoverConfig& playcover_config);
    bool select_gamepad(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::GamepadConfig& gamepad_config);
    bool select_linux(const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::LinuxConfig& linux_config);
    bool select_wlroots();
    bool select_wlroots_auto_detect();
    bool select_wlroots_manual_input();
    bool input_uinput_width_height();

    bool select_resource();
    bool add_task();
    bool add_default_tasks();
    void edit_task();
    bool delete_task();
    bool move_task();
    bool apply_preset();

    bool process_level_options(
        const std::vector<std::string>& option_names,
        std::vector<MAA_PROJECT_INTERFACE_NS::Configuration::Option>& config_options,
        const std::string& level_label);

    bool ensure_pretask_options();

    bool ensure_runtime_options();

    bool ensure_task_options();

    bool ensure_declared_option_tree(
        const std::vector<std::string>& option_names,
        const std::string& context_display_name,
        std::vector<MAA_PROJECT_INTERFACE_NS::Configuration::Option>& config_options,
        bool auto_accept_default);

    bool select_runtime_option_cases(
        const std::string& option_name,
        const std::vector<MAA_PROJECT_INTERFACE_NS::Configuration::Option>& config_options,
        std::vector<const MAA_PROJECT_INTERFACE_NS::InterfaceData::Option::Case*>& selected_cases) const;

    // Process option and its nested sub-options recursively
    // Returns false if option processing failed (e.g., option not found, invalid configuration)
    // auto_accept_default: 自动补全流程（default_check 默认任务、pretask 选项树）直接采用 default_case，
    // 不打断批量添加与 -d 直跑；用户主动配置时该值为 false，default_case 仅作为预选值仍会提示
    bool process_option(
        const std::string& option_name,
        const std::string& task_display_name,
        std::vector<MAA_PROJECT_INTERFACE_NS::Configuration::Option>& config_options,
        bool auto_accept_default = false);

    void print_config_tasks(bool with_index = true) const;

    bool check_validity();

    bool save_config();

    bool mpause();

    static std::string format_win32_config(const MAA_PROJECT_INTERFACE_NS::Configuration::Win32Config& win32_config);
    static std::string format_gamepad_config(const MAA_PROJECT_INTERFACE_NS::Configuration::GamepadConfig& gamepad_config);

    // 获取翻译后的显示名称：优先使用翻译后的 label，否则使用 name
    std::string get_display_name(const std::string& name, const std::string& label) const;

    std::string display_input_value(const std::string& option_name, const std::string& input_name, const std::string& value) const;

    // 读取文本内容：如果是文件路径则读取文件，否则直接返回；支持翻译
    std::string read_text_content(const std::string& text) const;

    // 查找当前配置中选中的 Controller 定义
    const MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller* find_current_controller() const;

    enum class ElevationResult
    {
        NotNeeded,       // 不需要提权
        Failed,          // 提权失败
        ElevatedStarted, // 已启动提权进程,当前进程应退出
    };

    // 在运行前检查是否需要管理员权限，如需提权则保存配置并重启
    ElevationResult check_and_elevate_if_needed();

private:
    MAA_PROJECT_INTERFACE_NS::Configurator config_;
    std::filesystem::path user_path_;
    bool input_aborted_ = false;
};
