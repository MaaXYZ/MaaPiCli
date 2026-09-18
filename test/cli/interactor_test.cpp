#include "CLI/interactor.h"

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
    StreamRedirector()
        : old_input_(std::cin.rdbuf(nullptr))
        , old_output_(std::cout.rdbuf(nullptr))
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
        "runtime-child": { "type": "input" },
        "runtime-second": { "type": "input" },
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
        { "name": "runtime-parent", "value": "on" },
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
    "resource": [ { "name": "default-resource", "path": [] } ],
    "task": [ { "name": "direct-task", "entry": "DirectTask", "option": [ "direct-option", "later-option" ] } ],
    "option": {
        "direct-option": {
            "type": "select",
            "default_case": "on",
            "cases": [ { "name": "on", "option": [ "direct-child" ] }, { "name": "off" } ]
        }
        ,
        "direct-child": { "type": "input" },
        "later-option": { "type": "input" }
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
            saved_config && saved_config->task.size() == 1 && saved_config->task.front().option.size() == 3
                && saved_config->task.front().option.at(0).value == "on" && saved_config->task.front().option.at(1).name == "direct-child"
                && saved_config->task.front().option.at(2).name == "later-option",
            "a missing direct-task subtree should be completed automatically in declaration order");

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
