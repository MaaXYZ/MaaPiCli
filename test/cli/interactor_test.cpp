#include "CLI/interactor.h"

#include "ProjectInterface/Configurator.h"
#include "ProjectInterface/Parser.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{
int failures = 0;

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path unique_temp_directory()
{
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto path = std::filesystem::temp_directory_path() / ("maapicli-eof-" + unique);
    std::filesystem::remove_all(path);
    return path;
}

class StreamRedirector
{
public:
    explicit StreamRedirector(std::string input = { })
        : old_input_(std::cin.rdbuf(nullptr))
        , old_output_(std::cout.rdbuf(nullptr))
        , input_stream_(std::move(input))
    {
        std::cin.rdbuf(input_stream_.rdbuf());
        std::cout.rdbuf(output_stream_.rdbuf());
    }

    ~StreamRedirector()
    {
        std::cin.rdbuf(old_input_);
        std::cout.rdbuf(old_output_);
    }

    const std::stringstream& output() const { return output_stream_; }

private:
    std::streambuf* old_input_;
    std::streambuf* old_output_;
    std::stringstream input_stream_;
    std::stringstream output_stream_;
};
}

int main()
{
    const std::filesystem::path fixture_dir = MAAPICLI_TEST_FIXTURE_DIR;
    const auto interface = MAA_PROJECT_INTERFACE_NS::Parser::parse_interface(fixture_dir / "interface.json");
    require(interface.has_value() && interface->controller.size() == 2, "the EOF fixture should contain two controllers");

    const auto user_dir = unique_temp_directory();

    {
        StreamRedirector redirector;

        Interactor interactor(user_dir);
        require(interactor.load(fixture_dir), "the EOF interface fixture should load");
        const bool completed = interactor.interact();
        if (!completed) {
            std::cerr << redirector.output().str();
        }
        require(completed, "EOF during first-time setup should exit successfully");
    }

    require(
        !std::filesystem::exists(user_dir / "config/maa_pi_config.json"),
        "EOF during first-time setup should not create a configuration");

    std::filesystem::remove_all(user_dir);

    {
        const auto resource_dir = unique_temp_directory();
        const auto user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);
        std::filesystem::create_directories(user_dir / "config");

        {
            std::ofstream interface_stream(resource_dir / "interface.json");
            interface_stream << R"json(
{
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [ { "name": "default-resource", "path": [ "resource" ] } ],
    "global_option": [ "runtime-parent", "runtime-second", "inactive-global-option" ],
    "option": {
        "runtime-parent": {
            "type": "select",
            "default_case": "on",
            "cases": [ { "name": "on", "option": [ "runtime-child" ] } ]
        },
        "runtime-child": {
            "type": "input",
            "inputs": [
                { "name": "child-value", "default": "child-default" },
                { "name": "child-secret", "default": "child-secret", "password": true }
            ]
        },
        "runtime-second": { "type": "input", "inputs": [ { "name": "second-value", "default": "second-default" } ] },
        "inactive-global-option": { "type": "input", "controller": [ "Win32" ] },
        "stale-resource-option": { "type": "input" },
        "stale-runtime-child": { "type": "input" }
    }
}
)json";
        }

        {
            std::ofstream config_stream(user_dir / "config" / "maa_pi_config.json");
            config_stream << R"json(
{
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "resource_option": [ { "name": "stale-resource-option" } ],
    "global_option": [
        { "name": "runtime-parent" },
        { "name": "runtime-second" },
        { "name": "inactive-global-option" },
        { "name": "stale-runtime-child" }
    ]
}
)json";
        }

        {
            StreamRedirector redirector;
            Interactor interactor(user_dir);
            require(interactor.load(resource_dir), "the runtime completion fixture should load");
            // A taskless interface intentionally fails runtime generation after ensure_runtime_options saves the config.
            require(!interactor.run(), "taskless runtime generation should fail after completing options");
        }

        const auto saved_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(user_dir / "config" / "maa_pi_config.json");
        require(saved_config.has_value(), "the completed runtime configuration should be saved");
        if (saved_config) {
            const auto names_of = [](const auto& options) {
                std::vector<std::string> names;
                names.reserve(options.size());
                for (const auto& option : options) {
                    names.emplace_back(option.name);
                }
                return names;
            };
            require(
                names_of(saved_config->global_option) == std::vector<std::string> { "runtime-parent", "runtime-child", "runtime-second" },
                "a missing nested runtime option should be completed without changing sibling order");
            const auto& saved_child_inputs = saved_config->global_option.at(1).inputs;
            require(
                saved_child_inputs.contains("child-secret") && saved_child_inputs.at("child-secret") != "child-secret",
                "a missing password Input default should be stored encrypted");
            require(
                saved_config->global_option.at(2).inputs
                    == std::unordered_map<std::string, std::string> { { "second-value", "second-default" } },
                "missing standalone Input options should use declared defaults during automatic completion");

            MAA_PROJECT_INTERFACE_NS::Configurator reloaded;
            require(reloaded.load(resource_dir, user_dir), "the configuration with automatic Input defaults should reload");
            require(
                reloaded.configuration().global_option.at(1).inputs
                    == std::unordered_map<std::string, std::string> { { "child-value", "child-default" },
                                                                      { "child-secret", "child-secret" } },
                "missing Input fields should use declared defaults during automatic completion");
            require(saved_config->resource_option.empty(), "an empty runtime option declaration list should be cleaned");
        }

        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove_all(user_dir);
    }

    {
        const auto resource_dir = unique_temp_directory();
        const auto user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);
        std::filesystem::create_directories(user_dir / "config");

        {
            std::ofstream interface_stream(resource_dir / "interface.json");
            interface_stream << R"json(
{
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [
        { "name": "default-resource", "path": [ "resource" ] },
        { "name": "other-resource", "path": [ "resource" ] }
    ],
    "task": [
        { "name": "active-task", "entry": "ActiveTask", "option": [ "active-option" ] },
        { "name": "other-controller-task", "entry": "OtherController", "controller": [ "other-controller" ] },
        { "name": "other-resource-task", "entry": "OtherResource", "resource": [ "other-resource" ] }
    ],
    "option": {
        "active-option": { "type": "input" },
        "inactive-option": { "type": "input" }
    }
}
)json";
        }

        {
            std::ofstream config_stream(user_dir / "config" / "maa_pi_config.json");
            config_stream << R"json(
{
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": [
        { "name": "active-task", "option": [ { "name": "active-option" } ] },
        { "name": "other-controller-task" },
        { "name": "other-resource-task" }
    ]
}
)json";
        }

        MAA_PROJECT_INTERFACE_NS::Configurator configurator;
        require(configurator.load(resource_dir, user_dir), "the runtime task filtering fixture should load");
        require(configurator.check_configuration(), "the runtime task filtering configuration should be valid");
        const auto runtime = configurator.generate_runtime();
        require(runtime.has_value(), "the runtime task filtering configuration should generate");
        if (runtime) {
            require(
                runtime->task.size() == 1 && runtime->task.front().name == "active-task",
                "tasks from other controller or resource contexts should not generate");
        }

        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove_all(user_dir);
    }

    {
        const auto resource_dir = unique_temp_directory();
        const auto user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);
        std::filesystem::create_directories(user_dir / "config");

        {
            std::ofstream interface_stream(resource_dir / "interface.json");
            interface_stream << R"json(
{
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [ { "name": "default-resource", "path": [] } ],
    "task": [
        { "name": "direct-task", "entry": "DirectTask", "option": [ "direct-option", "later-option" ] },
        { "name": "inactive-task", "entry": "InactiveTask", "controller": [ "other-controller" ], "option": [ "inactive-option" ] }
    ],
    "option": {
        "direct-option": {
            "type": "select",
            "default_case": "on",
            "cases": [ { "name": "on", "option": [ "direct-child" ] }, { "name": "off" } ]
        }
        ,
        "direct-child": { "type": "input" },
        "later-option": { "type": "input" },
        "inactive-option": { "type": "input" }
    }
}
)json";
        }

        {
            std::ofstream config_stream(user_dir / "config" / "maa_pi_config.json");
            config_stream << R"json(
{
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": [
        {
            "name": "direct-task",
            "option": [ { "name": "direct-option", "value": "on" }, { "name": "later-option" } ]
        },
        {
            "name": "inactive-task"
        }
    ]
}
)json";
        }

        {
            StreamRedirector redirector;
            Interactor interactor(user_dir);
            require(interactor.load(resource_dir), "the direct task completion fixture should load");
            // Runtime generation can still fail without a real resource; option completion must happen first without input.
            require(!interactor.run(), "direct execution after task completion should fail without a real runtime");
        }

        const auto saved_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(user_dir / "config" / "maa_pi_config.json");
        require(saved_config.has_value(), "the direct-task configuration should be saved");
        require(
            saved_config && saved_config->task.size() == 2 && saved_config->task.front().option.size() == 3
                && saved_config->task.front().option.at(0).value == "on" && saved_config->task.front().option.at(1).name == "direct-child"
                && saved_config->task.front().option.at(2).name == "later-option",
            "a missing direct-task subtree should be completed automatically in declaration order");
        require(
            saved_config && saved_config->task.size() == 2 && saved_config->task.at(1).name == "inactive-task"
                && saved_config->task.at(1).option.empty(),
            "task option completion should preserve inactive tasks without generating their options");

        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove_all(user_dir);
    }

    {
        const auto resource_dir = unique_temp_directory();
        const auto user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);
        std::filesystem::create_directories(user_dir / "config");

        {
            std::ofstream interface_stream(resource_dir / "interface.json");
            interface_stream << R"json(
{
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [
        { "name": "initial-resource", "path": [ "resource" ] },
        { "name": "target-resource", "path": [ "resource" ] }
    ],
    "task": [{ "name": "later-active-task", "entry": "LaterActive", "resource": [ "target-resource" ], "option": [ "later-active-option" ] }],
    "option": {
        "later-active-option": {
            "type": "select",
            "default_case": "on",
            "cases": [ { "name": "on", "option": [ "later-active-child" ] }, { "name": "off" } ]
        },
        "later-active-child": { "type": "input" }
    }
}
)json";
        }

        {
            std::ofstream config_stream(user_dir / "config" / "maa_pi_config.json");
            config_stream << R"json(
{
    "controller": { "name": "adb-controller" },
    "resource": "initial-resource",
    "task": [
        {
            "name": "later-active-task",
            "option": [ { "name": "later-active-option", "value": "stale" } ]
        }
    ]
}
)json";
        }

        {
            StreamRedirector redirector("2\n2\n6\n\n");
            Interactor interactor(user_dir);
            require(interactor.load(resource_dir), "the stale inactive task fixture should load");
            require(interactor.interact(), "switching resources should recover a stale inactive task option");
        }

        const auto saved_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(user_dir / "config" / "maa_pi_config.json");
        require(saved_config.has_value(), "the recovered task configuration should be saved");
        require(
            saved_config && saved_config->task.size() == 1 && saved_config->task.front().option.size() == 2
                && saved_config->task.front().option.at(0).value == "on"
                && saved_config->task.front().option.at(1).name == "later-active-child",
            "a stale task option should be recreated automatically after activation");

        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove_all(user_dir);
    }

    if (failures != 0) {
        std::cerr << failures << " interactor test assertion(s) failed\n";
        return 1;
    }

    std::cout << "MaaPiCli interactor tests passed\n";
    return 0;
}
