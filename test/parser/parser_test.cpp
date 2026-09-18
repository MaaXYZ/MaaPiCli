#include "ProjectInterface/Parser.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace
{
int failures = 0;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        ++failures;
    }
}

template <typename T, typename Member>
std::vector<std::string> names(const std::vector<T>& values, Member member)
{
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(value.*member);
    }
    return result;
}

bool same_names(const std::vector<std::string>& actual, const std::vector<std::string>& expected, const std::string& message)
{
    bool equal = actual == expected;
    require(equal, message);
    if (!equal) {
        std::cerr << "  expected:";
        for (const auto& name : expected) {
            std::cerr << ' ' << name;
        }
        std::cerr << "\n  actual:  ";
        for (const auto& name : actual) {
            std::cerr << ' ' << name;
        }
        std::cerr << '\n';
    }
    return equal;
}
} // namespace

int main()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const std::filesystem::path fixture_dir = MAAPICLI_TEST_FIXTURE_DIR;
    {
        InterfaceData data;
        data.interface_version = 2;
        data.resource.emplace_back().name = "default-resource";
        data.controller.emplace_back().name = "default-controller";
        data.option["valid-option"].cases.emplace_back().name = "fast";
        auto& checkbox = data.option["valid-checkbox"];
        checkbox.type = InterfaceData::Option::Type::Checkbox;
        checkbox.cases.emplace_back().name = "one";
        checkbox.cases.emplace_back().name = "two";
        checkbox.min_count = 1;
        checkbox.max_count = 1;
        checkbox.default_case = std::vector<std::string> { "one" };
        data.option["valid-input"].type = InterfaceData::Option::Type::Input;
        data.option["valid-input"].inputs.emplace_back().name = "current";
        data.pretask = std::vector<InterfaceData::Pretask> {
            InterfaceData::Pretask { .exec = "first-pretask", .name = "ordered-first", .option = { "valid-option", "valid-input" } }
        };

        Configuration config;
        config.resource = "default-resource";
        config.controller.name = "default-controller";
        config.controller.type = InterfaceData::Controller::Type::Adb;
        config.controller_option = {
            Configuration::Option { .name = "valid-checkbox", .values = { "stale-case", "one" } },
            Configuration::Option { .name = "valid-option", .value = "fast" },
        };
        config.pretask.emplace_back();
        config.pretask.front().name = "ordered-first";
        config.pretask.front().option = { Configuration::Option { .name = "stale-option" },
                                          Configuration::Option { .name = "valid-option", .value = "fast" },
                                          Configuration::Option { .name = "valid-input",
                                                                  .inputs = { { "stale-input", "old" }, { "current", "new" } } } };

        require(!Parser::check_configuration(data, config), "an invalid pretask option should mark the configuration as changed");
        require(config.pretask.size() == 1, "an invalid pretask option should not remove the entire pretask");
        require(config.pretask.front().option.size() == 2, "an invalid pretask option should be removed while valid options are retained");
        require(
            config.pretask.front().option.back().inputs.size() == 1 && config.pretask.front().option.back().inputs.contains("current"),
            "a stale input should be removed while current inputs are retained");

        config.controller_option.insert(
            config.controller_option.begin(),
            Configuration::Option { .name = "valid-checkbox", .values = { "stale-case", "one" } });
        require(!Parser::check_configuration(data, config), "an invalid checkbox count should mark the configuration as changed");
        require(config.controller_option.size() == 1, "an invalid checkbox option should be removed");
        require(
            config.controller_option.front().name == "valid-option" && config.controller_option.front().value == "fast",
            "valid controller options should be retained while checkbox constraints are cleaned up");
    }

    {
        InterfaceData data;
        data.interface_version = 2;
        data.resource.emplace_back().name = "default-resource";
        data.controller.emplace_back().name = "default-controller";
        auto& checkbox = data.option["duplicate-checkbox"];
        checkbox.type = InterfaceData::Option::Type::Checkbox;
        checkbox.cases.emplace_back().name = "one";
        checkbox.cases.emplace_back().name = "two";
        checkbox.min_count = 1;
        checkbox.max_count = 2;
        auto& duplicate_task = data.task.emplace_back();
        duplicate_task.name = "preset-task";
        duplicate_task.option = { "duplicate-checkbox" };

        Configuration config;
        config.resource = "default-resource";
        config.controller.name = "default-controller";
        config.controller.type = InterfaceData::Controller::Type::Adb;
        config.controller_option = { Configuration::Option { .name = "duplicate-checkbox", .values = { "one", "one" } } };
        config.task = { Configuration::Task {
            .name = "preset-task",
            .option = { Configuration::Option { .name = "duplicate-checkbox", .values = { "two", "two" } } } } };

        require(!Parser::check_configuration(data, config), "duplicate checkbox selections should mark the configuration as changed");
        require(config.controller_option.size() == 1, "a duplicated valid checkbox selection should be retained");
        require(
            config.controller_option.front().values == std::vector<std::string> { "one" },
            "checkbox selections should be normalized to unique names");
        require(config.task.size() == 1, "a duplicated valid task checkbox selection should be retained");
        require(
            config.task.front().option.size() == 1 && config.task.front().option.front().values == std::vector<std::string> { "two" },
            "task checkbox selections should be normalized to unique names");
        config.controller_option.clear();
        config.task.front().option.front().values = { "two", "two" };
        require(
            !Parser::check_configuration(data, config),
            "a duplicated task checkbox selection should mark only the task option as changed");
        require(
            config.task.front().option.front().values == std::vector<std::string> { "two" },
            "task checkbox selections should remain normalized without unrelated configuration changes");
        require(Parser::check_configuration(data, config), "normalized checkbox selections should be valid");
    }

    {
        InterfaceData data;
        data.interface_version = 2;
        data.resource.emplace_back().name = "default-resource";
        data.controller.emplace_back().name = "default-controller";
        data.task.emplace_back().name = "preset-task";
        auto& checkbox = data.option["task-checkbox"];
        checkbox.type = InterfaceData::Option::Type::Checkbox;
        checkbox.cases.emplace_back().name = "one";
        checkbox.cases.emplace_back().name = "two";
        checkbox.cases.emplace_back().name = "three";
        checkbox.min_count = 2;
        checkbox.max_count = 2;
        checkbox.cases.front().option = { "one-child" };
        data.option["stale-child"].type = InterfaceData::Option::Type::Input;
        data.option["one-child"].type = InterfaceData::Option::Type::Input;
        auto& valid_option = data.option["valid-option"];
        valid_option.cases.emplace_back().name = "fast";
        data.task.front().option = { "task-checkbox", "valid-option" };

        Configuration config;
        config.resource = "default-resource";
        config.controller.name = "default-controller";
        config.controller.type = InterfaceData::Controller::Type::Adb;
        auto& config_task = config.task.emplace_back();
        config_task.name = "preset-task";

        config_task.option = { Configuration::Option { .name = "task-checkbox", .values = { "one" } },
                               Configuration::Option { .name = "stale-child" },
                               Configuration::Option { .name = "valid-option", .value = "fast" } };
        require(!Parser::check_configuration(data, config), "an insufficient task checkbox count should mark configuration changed");
        require(config.task.size() == 1 && config.task.front().name == "preset-task", "an invalid checkbox count should retain the task");
        require(
            config_task.option.size() == 1 && config_task.option.front().name == "valid-option",
            "an invalid checkbox parent should remove its nested subtree while retaining later options");

        config_task.option = { Configuration::Option { .name = "task-checkbox", .values = { "one", "two", "three" } },
                               Configuration::Option { .name = "stale-child" },
                               Configuration::Option { .name = "valid-option", .value = "fast" } };
        require(!Parser::check_configuration(data, config), "an excessive task checkbox count should mark configuration changed");
        require(
            config.task.size() == 1 && config_task.option.size() == 1 && config_task.option.front().name == "valid-option",
            "an invalid checkbox count should retain the task");

        config_task.option = { Configuration::Option { .name = "task-checkbox", .values = { "one", "two" } },
                               Configuration::Option { .name = "stale-child" },
                               Configuration::Option { .name = "valid-option", .value = "fast" } };
        require(!Parser::check_configuration(data, config), "a stale child under a valid parent should mark configuration changed");
        require(config.task.size() == 1 && config_task.name == "preset-task", "a stale child should retain the task");
        require(
            config_task.option.size() == 1 && config_task.option.front().name == "valid-option",
            "a stale child should remove its parent subtree while retaining later options");

        config_task.option = { Configuration::Option { .name = "one-child" },
                               Configuration::Option { .name = "task-checkbox", .values = { "one", "two" } },
                               Configuration::Option { .name = "one-child" },
                               Configuration::Option { .name = "valid-option", .value = "fast" } };
        require(!Parser::check_configuration(data, config), "a nested option at task level should mark configuration changed");
        require(
            config_task.option.size() == 3 && config_task.option.front().name == "task-checkbox"
                && config_task.option.at(1).name == "one-child",
            "a nested option at task level should be removed while its parent subtree is retained");

        config_task.option = { Configuration::Option { .name = "task-checkbox", .values = { "one", "two" } },
                               Configuration::Option { .name = "one-child" },
                               Configuration::Option { .name = "stale-child" },
                               Configuration::Option { .name = "valid-option", .value = "fast" } };
        require(!Parser::check_configuration(data, config), "an extra stale child should mark configuration changed");
        require(
            config_task.option.size() == 1 && config_task.option.front().name == "valid-option",
            "an extra stale child should invalidate the preceding task option subtree");

        config_task.option = { Configuration::Option { .name = "task-checkbox", .values = { "one", "two" } },
                               Configuration::Option { .name = "one-child" },
                               Configuration::Option { .name = "valid-option", .value = "fast" } };
        require(Parser::check_configuration(data, config), "a valid task checkbox selection should pass");
        require(
            config.task.front().option.size() == 3 && config.task.front().option.at(1).name == "one-child",
            "a valid task checkbox subtree should be retained");
    }

    auto interface = Parser::parse_interface(fixture_dir / "interface.json");
    require(interface.has_value(), "valid interface with imports should parse");
    if (interface) {
        same_names(
            names(interface->task, &InterfaceData::Task::name),
            { "main-task", "first-import-task", "second-import-task" },
            "tasks should append in main-first import order");

        same_names(
            names(interface->preset, &InterfaceData::Preset::name),
            { "main-preset", "first-preset", "second-preset" },
            "presets should append in main-first import order");

        same_names(
            names(interface->setting, &InterfaceData::Setting::name),
            { "main-setting", "first-setting", "second-setting" },
            "settings should append in main-first import order");

        same_names(
            interface->global_option,
            { "first-global", "shared-global", "first-only-global", "second-global", "second-only-global" },
            "global options should append and retain the first occurrence");

        require(
            interface->resource.size() == 1 && interface->resource.front().hash == "Expected-Resource-Hash",
            "resource hash should parse");

        same_names(
            names(interface->group, &InterfaceData::Group::name),
            { "main-group", "first-group", "first-only-group", "second-only-group" },
            "groups should append and retain the first occurrence");

        auto shared_option = interface->option.find("shared-option");
        require(
            shared_option != interface->option.end() && shared_option->second.cases.size() == 1
                && shared_option->second.cases.front().label == "second-import-label",
            "later imports should override options with the same name");
        require(interface->option.contains("first-import-option"), "main task should be able to reference an imported option");

        auto password_option = interface->option.find("password-option");
        require(
            password_option != interface->option.end() && password_option->second.inputs.size() == 2
                && password_option->second.inputs.front().password && !password_option->second.inputs.back().password,
            "input fields should parse the password flag");

        auto constrained_option = interface->option.find("checkbox-constrained");
        require(
            constrained_option != interface->option.end() && constrained_option->second.min_count == 1u
                && constrained_option->second.max_count == 2u,
            "checkbox count constraints should parse");

        require(interface->pretask.has_value(), "merged pretask should be present");
        if (interface->pretask) {
            const auto* pretasks = std::get_if<std::vector<InterfaceData::Pretask>>(&*interface->pretask);
            require(pretasks != nullptr, "merged pretask should use the array variant");
            if (pretasks) {
                same_names(
                    names(*pretasks, &InterfaceData::Pretask::name),
                    { "main-first", "main-second", "first-single-object", "second-first", "second-second" },
                    "object and array pretasks should flatten in main-first import order");
            }
        }
    }

    require(
        !Parser::parse_interface(fixture_dir / "invalid_option.json").has_value(),
        "a missing merged option reference should be rejected");
    require(
        !Parser::parse_interface(fixture_dir / "invalid_group.json").has_value(),
        "a missing merged group reference should be rejected");
    require(
        !Parser::parse_interface(fixture_dir / "invalid_option_constraints.json").has_value(),
        "invalid checkbox bounds should be rejected");
    require(
        !Parser::parse_interface(fixture_dir / "invalid_option_defaults.json").has_value(),
        "checkbox defaults outside min/max count should be rejected");

    auto zero_max_json = json::parse(
        R"json({
            "interface_version": 2,
            "controller": [{ "name": "default-controller", "type": "Adb" }],
            "resource": [{ "name": "default-resource", "path": ["resource"] }],
            "agent": [{ "child_exec": "agent-server" }],
            "option": {
                "empty-checkbox": {
                    "type": "checkbox",
                    "cases": [{ "name": "one" }],
                    "max_count": 0
                }
            }
        })json");
    require(zero_max_json.has_value(), "zero checkbox maximum fixture should parse as JSON");
    require(
        zero_max_json && Parser::parse_interface(*zero_max_json).has_value(),
        "max_count zero should accept an empty checkbox selection");

    auto linux_interface_json = json::parse(
        R"json({
            "interface_version": 2,
            "controller": [{
                "name": "linux-controller",
                "type": "linux",
                "display_expand": [1280, 720],
                "linux": {
                    "screencap": "PipeWire",
                    "input": "UInput",
                    "pipewire_source": "Portal",
                    "use_win32_vk_code": true
                }
            }],
            "resource": [{ "name": "default-resource", "path": ["resource"] }],
            "agent": [{ "child_exec": "agent-server" }]
        })json");
    require(linux_interface_json.has_value(), "Linux interface fixture should parse as JSON");
    auto linux_interface = linux_interface_json ? Parser::parse_interface(*linux_interface_json) : std::nullopt;
    require(linux_interface.has_value(), "Linux interface fields should parse");
    if (linux_interface) {
        const auto& linux_controller = linux_interface->controller.front();
        require(linux_controller.type == InterfaceData::Controller::Type::Linux, "Linux controller type should parse");
        require(
            linux_controller.lnx.screencap == "PipeWire" && linux_controller.lnx.input == "UInput"
                && linux_controller.lnx.pipewire_source == "Portal" && linux_controller.lnx.use_win32_vk_code,
            "Linux controller settings should use the linux JSON key");
        require(
            linux_controller.display_expand.has_value() && (*linux_controller.display_expand)[0] == 1280
                && (*linux_controller.display_expand)[1] == 720,
            "display_expand should parse");
    }

    auto invalid_pipewire_source_json = json::parse(
        R"json({
            "interface_version": 2,
            "controller": [{
                "name": "linux-controller",
                "type": "Linux",
                "linux": {
                    "screencap": "PipeWire",
                    "pipewire_source": "portal"
                }
            }],
            "resource": [{ "name": "default-resource", "path": ["resource"] }]
        })json");
    require(invalid_pipewire_source_json.has_value(), "invalid PipeWire source fixture should parse as JSON");
    require(
        !(invalid_pipewire_source_json && Parser::parse_interface(*invalid_pipewire_source_json).has_value()),
        "an unsupported PipeWire source should be rejected");

    auto invalid_linux_screencap_json = json::parse(
        R"json({
            "interface_version": 2,
            "controller": [{
                "name": "linux-controller",
                "type": "Linux",
                "linux": {
                    "screencap": "Pipewire",
                    "input": "UInput"
                }
            }],
            "resource": [{ "name": "default-resource", "path": ["resource"] }]
        })json");
    require(invalid_linux_screencap_json.has_value(), "invalid Linux screencap fixture should parse as JSON");
    require(
        !(invalid_linux_screencap_json && Parser::parse_interface(*invalid_linux_screencap_json).has_value()),
        "an unknown Linux screencap method should be rejected");

    auto invalid_linux_input_json = json::parse(
        R"json({
            "interface_version": 2,
            "controller": [{
                "name": "linux-controller",
                "type": "Linux",
                "linux": {
                    "screencap": "PipeWire",
                    "input": "Uinput"
                }
            }],
            "resource": [{ "name": "default-resource", "path": ["resource"] }]
        })json");
    require(invalid_linux_input_json.has_value(), "invalid Linux input fixture should parse as JSON");
    require(
        !(invalid_linux_input_json && Parser::parse_interface(*invalid_linux_input_json).has_value()),
        "an unknown Linux input method should be rejected");

    auto conflicting_display_options_json = json::parse(
        R"json({
            "interface_version": 2,
            "controller": [{
                "name": "default-controller",
                "type": "Adb",
                "display_short_side": 720,
                "display_raw": true
            }],
            "resource": [{ "name": "default-resource", "path": ["resource"] }]
        })json");
    require(conflicting_display_options_json.has_value(), "conflicting display option fixture should parse as JSON");
    require(
        !(conflicting_display_options_json && Parser::parse_interface(*conflicting_display_options_json).has_value()),
        "conflicting display options should be rejected");

    auto cyclic_options_json = json::parse(
        R"json({
            "interface_version": 2,
            "controller": [{ "name": "default-controller", "type": "Adb" }],
            "resource": [{ "name": "default-resource", "path": ["resource"] }],
            "option": {
                "option-a": {
                    "type": "select",
                    "cases": [{ "name": "one", "option": ["option-b"] }]
                },
                "option-b": {
                    "type": "select",
                    "cases": [{ "name": "one", "option": ["option-a"] }]
                }
            }
        })json");
    require(cyclic_options_json.has_value(), "cyclic option fixture should parse as JSON");
    require(
        !(cyclic_options_json && Parser::parse_interface(*cyclic_options_json).has_value()),
        "a cyclic option graph should be rejected");

    auto legacy_wlroots_interface_json = json::parse(
        R"json({
            "interface_version": 2,
            "controller": [{
                "name": "linux-controller",
                "type": "WlRoots",
                "wlroots": {
                    "screencap": "Wlr",
                    "input": "UInput"
                }
            }],
            "resource": [{ "name": "default-resource", "path": ["resource"] }]
        })json");
    require(legacy_wlroots_interface_json.has_value(), "legacy WlRoots interface fixture should parse as JSON");
    auto legacy_wlroots_interface = legacy_wlroots_interface_json ? Parser::parse_interface(*legacy_wlroots_interface_json) : std::nullopt;
    require(legacy_wlroots_interface.has_value(), "a legacy WlRoots interface should migrate");
    if (legacy_wlroots_interface) {
        const auto& controller = legacy_wlroots_interface->controller.front();
        require(controller.type == InterfaceData::Controller::Type::Linux, "a legacy WlRoots controller should become Linux");
        require(
            controller.lnx.screencap == "Wlr" && controller.lnx.input == "UInput",
            "legacy WlRoots controller settings should migrate to linux");
    }

    auto linux_config_json = json::parse(
        R"json({
            "controller": { "name": "linux-controller", "type": "linux" },
            "resource": "",
            "task": [],
            "linux": {
                "wlr_socket_path": "/run/user/1000/wayland-0",
                "uinput_screen_width": 1280,
                "uinput_screen_height": 720,
                "eis_socket_path": "/run/user/1000/gamescope-0-ei"
            }
        })json");
    require(linux_config_json.has_value(), "Linux configuration fixture should parse as JSON");
    auto linux_config = linux_config_json ? Parser::parse_config(*linux_config_json) : std::nullopt;
    require(linux_config.has_value(), "Linux configuration fields should parse");
    if (linux_config) {
        require(
            linux_config->lnx.wlr_socket_path == "/run/user/1000/wayland-0" && linux_config->lnx.uinput_screen_width == 1280
                && linux_config->lnx.uinput_screen_height == 720 && linux_config->lnx.eis_socket_path == "/run/user/1000/gamescope-0-ei",
            "Linux user configuration should use the linux JSON key");

        auto roundtrip = linux_config->to_json();
        require(roundtrip.contains("linux"), "Linux user configuration should serialize as linux");
        require(!roundtrip.contains("wlroots"), "Linux user configuration should not serialize the legacy key");
    }

    {
        auto legacy_linux_config_json = json::parse(
            R"json({
            "controller": { "name": "linux-controller" },
            "resource": "",
            "task": [],
            "wlroots": {
                "wlr_socket_path": "/run/user/1000/wayland-0",
                "uinput_screen_width": 1280,
                "uinput_screen_height": 720,
                "eis_socket_path": "/run/user/1000/gamescope-0-ei"
            }
        })json");
        require(legacy_linux_config_json.has_value(), "legacy Linux configuration fixture should parse as JSON");
        auto legacy_linux_config = legacy_linux_config_json ? Parser::parse_config(*legacy_linux_config_json) : std::nullopt;
        require(legacy_linux_config.has_value(), "a legacy Linux user configuration should migrate");
        if (legacy_linux_config) {
            require(
                legacy_linux_config->lnx.wlr_socket_path == "/run/user/1000/wayland-0"
                    && legacy_linux_config->lnx.uinput_screen_width == 1280 && legacy_linux_config->lnx.uinput_screen_height == 720
                    && legacy_linux_config->lnx.eis_socket_path == "/run/user/1000/gamescope-0-ei",
                "legacy wlroots user configuration values should migrate to linux");
            auto legacy_roundtrip = legacy_linux_config->to_json();
            require(
                legacy_roundtrip.contains("linux") && !legacy_roundtrip.contains("wlroots"),
                "migrated Linux configuration should serialize only linux");
        }
    }

    if (failures != 0) {
        std::cerr << failures << " parser test assertion(s) failed\n";
        return 1;
    }

    std::cout << "MaaPiCli parser tests passed\n";
    return 0;
}
