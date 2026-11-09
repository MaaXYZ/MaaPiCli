#include "CLI/interactor.h"

#include "MaaUtils/Platform.h"
#include "ProjectInterface/Configurator.h"
#include "ProjectInterface/Parser.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#ifndef _WIN32
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <endian.h>
#include <linux/posix_acl_xattr.h>
#include <sys/xattr.h>
#endif
#include <unistd.h>
#endif

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

void test_preset_controller_filter(const std::filesystem::path& fixture_dir)
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto data = Parser::parse_interface(fixture_dir / "interface.json");
    require(
        data && data->preset.size() == 3 && data->preset.back().controller == std::vector<std::string> { "default-controller" },
        "imported presets should retain controller restrictions");

    const auto original_cwd = std::filesystem::current_path();
    for (const auto& controller : { "default-controller", "other-controller" }) {
        const auto root_dir = unique_temp_directory();
        const auto user_dir = root_dir / "user";
        const auto changed_cwd = root_dir / "changed-cwd";
        std::filesystem::create_directories(user_dir / "config");
        std::filesystem::create_directories(changed_cwd);
        const bool is_default = std::string_view(controller) == "default-controller";
        Configuration config;
        config.controller.name = controller;
        config.resource = "default-resource";
        config.task.emplace_back().name = is_default ? "original-task" : "preset-task";
        config.last_welcome = { "stale announcement" };
        config.last_resolved_welcome = config.last_welcome;
        {
            std::ofstream stream(user_dir / "config/maa_pi_config.json");
            stream << config.to_json().dumps();
        }

        {
            StreamRedirector redirector("7\n1\n8\n");
            std::filesystem::current_path(root_dir);
            Interactor interactor("user");
            require(interactor.load(fixture_dir), "controller-filtered preset fixture should load");
            std::filesystem::current_path(changed_cwd);
            require(interactor.interact(), "a visible preset should apply successfully");
            std::filesystem::current_path(original_cwd);
            const auto output = redirector.output().str();
            require(output.find(is_default ? "other-only" : "default-only") == std::string::npos, "inapplicable presets should be hidden");
            require(output.find(is_default ? "default-only" : "other-only") != std::string::npos, "applicable presets should be visible");
            require(output.find("unrestricted") != std::string::npos, "presets without controller restrictions should remain visible");
        }

        const auto saved = Parser::parse_config(user_dir / "config/maa_pi_config.json");
        const auto expected_task = is_default ? "preset-task" : "original-task";
        require(
            saved && saved->task.size() == 1 && saved->task.front().name == expected_task,
            "preset numbering should refer to the filtered list");
        require(
            saved && saved->last_welcome.empty() && saved->last_resolved_welcome.empty(),
            "welcome snapshots should update in the loaded user directory after cwd changes");
        require(!std::filesystem::exists(changed_cwd / "user"), "saving should not create a user directory relative to the changed cwd");
        std::filesystem::remove_all(root_dir);
    }

    const auto user_dir = unique_temp_directory();
    std::filesystem::create_directories(user_dir / "config");
    Configuration config;
    config.controller.name = "default-controller";
    config.resource = "default-resource";
    {
        std::ofstream stream(user_dir / "config/maa_pi_config.json");
        stream << config.to_json().dumps();
    }
    {
        StreamRedirector redirector("7\n");
        Interactor interactor(user_dir);
        require(interactor.load(fixture_dir / "no_match"), "a fixture with no applicable presets should load");
        require(interactor.interact(), "exit should retain its original index when no presets apply");
        require(redirector.output().str().find("Apply preset") == std::string::npos, "the menu should hide unavailable preset actions");
    }
    std::filesystem::remove_all(user_dir);
}

void test_path_inputs_and_agent_environment(const std::filesystem::path& fixture_dir)
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const auto user_dir = unique_temp_directory() / std::filesystem::path(u8"user 数据");
    std::filesystem::create_directories(user_dir / "config");
    Configuration config;
    config.controller.name = "default-controller";
    config.resource = "default-resource";
    {
        std::ofstream stream(user_dir / "config/maa_pi_config.json");
        stream << config.to_json().dumps();
    }

    const std::string file = "C:\\future folder\\配置.json";
    const auto directory = MAA_NS::path_to_utf8_string(user_dir / "future folder");
    {
        StreamRedirector redirector("3\n3\ninvalid.txt\n" + file + "\n" + directory + "\n\n\n\n8\n");
        Interactor interactor(user_dir);
        require(interactor.load(fixture_dir), "file and directory inputs should load without native dialogs");
        require(interactor.interact(), "manual path inputs should save successfully");
        const auto output = redirector.output().str();
        require(
            output.find("file path") != std::string::npos && output.find("directory path") != std::string::npos,
            "path inputs should show hints");
        require(output.find("Expected JSON file") != std::string::npos, "path inputs should retain regular expression validation");
    }

    Configurator reloaded;
    const auto original_cwd = std::filesystem::current_path();
    std::filesystem::current_path(user_dir.parent_path());
    const bool loaded = reloaded.load(fixture_dir, user_dir.filename());
    std::filesystem::current_path(original_cwd);
    require(loaded, "saved paths and a relative user directory should load");
    require(reloaded.configuration().task.size() == 1, "manual path input should add one task");
    if (!reloaded.configuration().task.empty() && !reloaded.configuration().task.front().option.empty()) {
        const auto& inputs = reloaded.configuration().task.front().option.front().inputs;
        require(inputs.at("file") == file && inputs.at("directory") == directory, "saved path strings should remain unchanged");
    }

    std::filesystem::current_path(fixture_dir);
    const auto runtime = reloaded.generate_runtime();
    std::filesystem::current_path(original_cwd);
    require(runtime.has_value(), "path inputs should generate a runtime without requiring paths to exist");
    if (runtime) {
        require(runtime->task.front().pipeline_override.as_array().size() == 2, "escaped path inputs should produce a pipeline override");
        if (runtime->task.front().pipeline_override.as_array().size() == 2) {
            const auto& node = runtime->task.front().pipeline_override.as_array().at(1).at("PathNode");
            require(
                node.at("file") == file && node.at("directory") == directory,
                "pipeline substitution should preserve native path separators");
            require(node.at("embedded") == "prefix:" + file, "embedded path placeholders should remain valid JSON");
            require(node.at("note") == "keep \"quoted\" text", "text input quotes should remain valid JSON");
            require(node.at("count") == 42 && node.at("enabled") == true, "input controls should retain pipeline type conversions");
        }

        require(runtime->agent.size() == 2, "all declared agents should be generated");
        for (const auto& agent : runtime->agent) {
            const auto directory_env = agent.env_vars.find("PI_USER_DATA_DIR");
            const auto directory_path =
                directory_env == agent.env_vars.end() ? std::filesystem::path { } : MAA_NS::path(directory_env->second);
            require(
                directory_path.is_absolute() && std::filesystem::equivalent(directory_path, user_dir),
                "each agent should receive the absolute user directory captured at load, independent of its resource cwd");
            require(agent.env_vars.at("PI_INTERFACE_VERSION") == "v2.11.0", "agents should receive the updated PI capability version");
        }
    }
    std::filesystem::remove_all(user_dir.parent_path());
}

#if defined(__APPLE__)
constexpr auto kTestAclType = ACL_TYPE_EXTENDED;

std::optional<std::string> configuration_acl_data(const std::filesystem::path& path)
{
    const auto acl = ::acl_get_file(path.c_str(), kTestAclType);
    if (acl == nullptr) {
        return std::nullopt;
    }

    char* text = ::acl_to_text(acl, nullptr);
    const std::optional<std::string> result = text ? std::optional<std::string>(text) : std::nullopt;
    ::acl_free(text);
    ::acl_free(acl);
    return result;
}

bool install_configuration_acl(const std::filesystem::path& path)
{
    acl_t acl = ::acl_init(1);
    if (acl == nullptr) {
        return false;
    }

    acl_entry_t entry = nullptr;
    acl_permset_t permissions = nullptr;
    uid_t uid = ::geteuid();
    const bool installed = ::acl_create_entry(&acl, &entry) == 0 && ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0
                           && ::acl_set_qualifier(entry, &uid) == 0 && ::acl_get_permset(entry, &permissions) == 0
                           && ::acl_clear_perms(permissions) == 0 && ::acl_add_perm(permissions, ACL_READ_DATA) == 0
                           && ::acl_valid(acl) == 0 && ::acl_set_file(path.c_str(), kTestAclType, acl) == 0;
    ::acl_free(acl);
    return installed;
}
#elif defined(__linux__)
constexpr char kPosixAclXattr[] = "system.posix_acl_access";
constexpr char kPosixDefaultAclXattr[] = "system.posix_acl_default";
constexpr uint16_t kAclUserObj = 0x01;
constexpr uint16_t kAclUser = 0x02;
constexpr uint16_t kAclGroupObj = 0x04;
constexpr uint16_t kAclMask = 0x10;
constexpr uint16_t kAclOther = 0x20;
constexpr uint16_t kAclRead = 0x04;
constexpr uint16_t kAclWrite = 0x02;
constexpr uint32_t kAclUndefinedId = 0xffffffff;

void append_le16(std::string& data, uint16_t value)
{
    const auto encoded = htole16(value);
    data.append(reinterpret_cast<const char*>(&encoded), sizeof(encoded));
}

void append_le32(std::string& data, uint32_t value)
{
    const auto encoded = htole32(value);
    data.append(reinterpret_cast<const char*>(&encoded), sizeof(encoded));
}

void append_acl_entry(std::string& data, uint16_t tag, uint16_t permissions, uint32_t qualifier)
{
    append_le16(data, tag);
    append_le16(data, permissions);
    append_le32(data, qualifier);
}

std::optional<std::string> configuration_acl_data(const std::filesystem::path& path)
{
    const auto size = ::getxattr(path.c_str(), kPosixAclXattr, nullptr, 0);
    if (size < 0) {
        return std::nullopt;
    }

    std::string data(static_cast<size_t>(size), '\0');
    const auto actual_size = ::getxattr(path.c_str(), kPosixAclXattr, data.data(), data.size());
    if (actual_size < 0 || actual_size != static_cast<ssize_t>(data.size())) {
        return std::nullopt;
    }
    return data;
}

bool install_configuration_acl(const std::filesystem::path& path)
{
    std::string acl;
    append_le32(acl, POSIX_ACL_XATTR_VERSION);
    append_acl_entry(acl, kAclUserObj, kAclRead | kAclWrite, kAclUndefinedId);
    append_acl_entry(acl, kAclUser, kAclRead, ::geteuid());
    append_acl_entry(acl, kAclGroupObj, 0, kAclUndefinedId);
    append_acl_entry(acl, kAclMask, kAclRead, kAclUndefinedId);
    append_acl_entry(acl, kAclOther, 0, kAclUndefinedId);

    return ::setxattr(path.c_str(), kPosixAclXattr, acl.data(), acl.size(), 0) == 0;
}

bool install_default_configuration_acl(const std::filesystem::path& directory)
{
    std::string acl;
    append_le32(acl, POSIX_ACL_XATTR_VERSION);
    append_acl_entry(acl, kAclUserObj, kAclRead | kAclWrite, kAclUndefinedId);
    append_acl_entry(acl, kAclUser, kAclRead, ::geteuid());
    append_acl_entry(acl, kAclGroupObj, 0, kAclUndefinedId);
    append_acl_entry(acl, kAclMask, kAclRead, kAclUndefinedId);
    append_acl_entry(acl, kAclOther, 0, kAclUndefinedId);

    return ::setxattr(directory.c_str(), kPosixDefaultAclXattr, acl.data(), acl.size(), 0) == 0;
}

bool remove_configuration_acl(const std::filesystem::path& path)
{
    return ::removexattr(path.c_str(), kPosixAclXattr) == 0;
}
#endif

}

int main()
{
    const std::filesystem::path fixture_dir = MAAPICLI_TEST_FIXTURE_DIR;
    test_preset_controller_filter(fixture_dir.parent_path() / "pi_extensions");
    test_path_inputs_and_agent_environment(fixture_dir.parent_path() / "pi_extensions");

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
    "task": [],
    "resource_option": [ { "name": "stale-resource-option" } ],
    "global_option": [
        { "name": "runtime-parent", "value": "on" },
        { "name": "runtime-child", "inputs": { "child-value": "old-value" } },
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
                saved_child_inputs.at("child-value") == "old-value" && saved_child_inputs.contains("child-secret")
                    && saved_child_inputs.at("child-secret") != "child-secret",
                "existing Input values should be preserved while missing password defaults are stored encrypted");
            require(
                saved_config->global_option.at(2).inputs
                    == std::unordered_map<std::string, std::string> { { "second-value", "second-default" } },
                "missing standalone Input options should use declared defaults during automatic completion");

            MAA_PROJECT_INTERFACE_NS::Configurator reloaded;
            require(reloaded.load(resource_dir, user_dir), "the configuration with automatic Input defaults should reload");
            require(
                reloaded.configuration().global_option.at(1).inputs
                    == std::unordered_map<std::string, std::string> { { "child-value", "old-value" }, { "child-secret", "child-secret" } },
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

    {
        const auto resource_dir = unique_temp_directory();
        const auto user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);
        std::filesystem::create_directories(user_dir / "config");

        {
            std::ofstream interface_stream(resource_dir / "interface.json");
            interface_stream << R"json({
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [ { "name": "default-resource", "path": [ "resource" ] } ],
    "welcome": "Welcome update"
}
)json";
        }

        {
            std::ofstream config_stream(user_dir / "config" / "maa_pi_config.json");
            config_stream << R"json({
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": [],
    "last_welcome": [ "old-welcome" ],
    "last_resolved_welcome": [ "Old welcome" ]
}
)json";
        }

        {
            StreamRedirector redirector("7\n");
            Interactor interactor(user_dir);
            require(interactor.load(resource_dir), "the welcome fixture should load");
            require(interactor.interact(), "exiting after a welcome update should succeed");
            require(redirector.output().str().find("Welcome update") != std::string::npos, "a changed welcome should be shown");
        }

        auto saved_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(user_dir / "config/maa_pi_config.json");
        require(saved_config.has_value(), "a shown welcome should be persisted immediately");
        require(
            saved_config && saved_config->last_welcome == std::vector<std::string> { "Welcome update" }
                && saved_config->last_resolved_welcome == std::vector<std::string> { "Welcome update" },
            "welcome snapshots should match the shown announcement");

        {
            StreamRedirector redirector("7\n");
            Interactor interactor(user_dir);
            require(interactor.load(resource_dir), "the persisted welcome fixture should reload");
            require(interactor.interact(), "exiting after an unchanged welcome should succeed");
            require(redirector.output().str().find("Welcome update") == std::string::npos, "an unchanged welcome should not be shown");
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
            interface_stream << R"json({
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [
        { "name": "default-resource", "path": [ "resource" ] },
        { "name": "other-resource", "path": [ "resource" ] }
    ],
    "task": [ { "name": "local-task", "entry": "LocalTask" } ],
    "welcome": "Welcome update"
}
)json";
        }

        {
            std::ofstream config_stream(user_dir / "config" / "maa_pi_config.json");
            config_stream << R"json({
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": [],
    "last_welcome": [ "old-welcome" ],
    "last_resolved_welcome": [ "Old welcome" ]
}
)json";
        }

        MAA_PROJECT_INTERFACE_NS::Configurator first;
        MAA_PROJECT_INTERFACE_NS::Configurator second;
        require(first.load(resource_dir, user_dir), "the first concurrent-save fixture should load");
        require(second.load(resource_dir, user_dir), "the second concurrent-save fixture should load");

        first.configuration().resource = "other-resource";
        require(first.save(user_dir), "the first local configuration edit should save");

        second.configuration().task.emplace_back(MAA_PROJECT_INTERFACE_NS::Configuration::Task { .name = "local-task" });
        require(second.save(user_dir), "the second local configuration edit should save");

        const auto changed = second.update_welcome_snapshots(user_dir, { "Welcome update" }, { "Welcome update" });
        require(changed.has_value() && *changed, "the changed welcome snapshot should save");

        const auto saved_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(user_dir / "config/maa_pi_config.json");
        require(
            saved_config && saved_config->resource == "other-resource" && saved_config->task.size() == 1
                && saved_config->task.front().name == "local-task",
            "independent local edits should be preserved together");
        require(
            saved_config && saved_config->last_welcome == std::vector<std::string> { "Welcome update" }
                && saved_config->last_resolved_welcome == std::vector<std::string> { "Welcome update" },
            "updating welcome snapshots should preserve newer configuration settings");
        require(
            !std::filesystem::exists(user_dir / "config/maa_pi_config.json.tmp"),
            "successful configuration writes should not leave a temporary file");

        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove_all(user_dir);
    }

#ifndef _WIN32
    {
        const auto resource_dir = unique_temp_directory();
        const auto user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);
        std::filesystem::create_directories(user_dir / "config");

        {
            std::ofstream interface_stream(resource_dir / "interface.json");
            interface_stream << R"json({
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [
        { "name": "default-resource", "path": [ "resource" ] },
        { "name": "other-resource", "path": [ "resource" ] }
    ]
}
)json";
        }

        const auto config_path = user_dir / "config/maa_pi_config.json";
        {
            std::ofstream config_stream(config_path);
            config_stream << R"json({
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": []
}
)json";
        }
        std::filesystem::permissions(
            config_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read);

        MAA_PROJECT_INTERFACE_NS::Configurator configurator;
        require(configurator.load(resource_dir, user_dir), "the restricted configuration fixture should load");
        configurator.configuration().resource = "other-resource";
        require(configurator.save(user_dir), "the restricted configuration fixture should save");

        const auto saved_permissions = std::filesystem::status(config_path).permissions();
        require(
            saved_permissions
                == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read),
            "saving a configuration should preserve its existing permissions");
        require(!std::filesystem::exists(config_path.string() + ".tmp"), "a permission-preserving save should clean its temporary file");

#if defined(__APPLE__) || defined(__linux__)
        const bool acl_installed = install_configuration_acl(config_path);
        const auto original_acl = acl_installed ? configuration_acl_data(config_path) : std::nullopt;

        if (original_acl.has_value()) {
            MAA_PROJECT_INTERFACE_NS::Configurator acl_configurator;
            require(acl_configurator.load(resource_dir, user_dir), "the ACL configuration fixture should reload");
            acl_configurator.configuration().resource = "default-resource";
            require(acl_configurator.save(user_dir), "the ACL configuration fixture should save");

            const auto saved_acl = configuration_acl_data(config_path);
            require(saved_acl.has_value() && *saved_acl == *original_acl, "saving a configuration should preserve its existing ACL");
            require(!std::filesystem::exists(config_path.string() + ".tmp"), "an ACL-preserving save should clean its temporary file");
        }
#if defined(__linux__)
        if (remove_configuration_acl(config_path) && install_default_configuration_acl(config_path.parent_path())
            && !configuration_acl_data(config_path).has_value()) {
            MAA_PROJECT_INTERFACE_NS::Configurator inherited_acl_configurator;
            require(inherited_acl_configurator.load(resource_dir, user_dir), "the inherited ACL configuration fixture should reload");
            inherited_acl_configurator.configuration().resource = "default-resource";
            require(inherited_acl_configurator.save(user_dir), "the inherited ACL configuration fixture should save");
            require(
                !configuration_acl_data(config_path).has_value(),
                "saving a configuration without an ACL should remove an inherited ACL");
        }
#endif
#endif

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
            interface_stream << R"json({
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [
        { "name": "default-resource", "path": [ "resource" ] },
        { "name": "other-resource", "path": [ "resource" ] }
    ]
}
)json";
        }

        const auto linked_config_path = user_dir / "config/maa_pi_config.json";
        const auto target_config_path = user_dir / "config/linked-config.json";
        {
            std::ofstream target_stream(target_config_path);
            target_stream << R"json({
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": []
}
)json";
        }
        std::filesystem::create_symlink("linked-config.json", linked_config_path);

        MAA_PROJECT_INTERFACE_NS::Configurator configurator;
        require(configurator.load(resource_dir, user_dir), "the symlinked configuration fixture should load");
        configurator.configuration().resource = "other-resource";
        require(configurator.save(user_dir), "the symlinked configuration fixture should save");

        require(
            std::filesystem::is_symlink(std::filesystem::symlink_status(linked_config_path)),
            "saving should preserve a configuration symlink");
        const auto saved_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(linked_config_path);
        require(saved_config && saved_config->resource == "other-resource", "a symlinked configuration target should be updated");
        require(
            !std::filesystem::exists(target_config_path.string() + ".tmp"),
            "a symlinked configuration save should clean its temporary file");

        const auto nested_directory = user_dir / "config/profiles";
        const auto nested_config_path = nested_directory / "nested-config.json";
        std::filesystem::create_directories(nested_directory);
        {
            std::ofstream nested_stream(nested_config_path);
            nested_stream << R"json({
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": []
}
)json";
        }
        std::filesystem::remove(linked_config_path);
        std::filesystem::create_symlink("profiles/nested-config.json", linked_config_path);

        MAA_PROJECT_INTERFACE_NS::Configurator nested_configurator;
        require(nested_configurator.load(resource_dir, user_dir), "the nested symlink configuration fixture should load");
        nested_configurator.configuration().resource = "other-resource";
        require(nested_configurator.save(user_dir), "the nested symlink configuration fixture should save");
        require(
            std::filesystem::is_symlink(std::filesystem::symlink_status(linked_config_path)),
            "saving should preserve a nested configuration symlink");
        const auto saved_nested_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(nested_config_path);
        require(
            saved_nested_config && saved_nested_config->resource == "other-resource",
            "a nested configuration target should be updated in place");
        require(
            !std::filesystem::exists(nested_config_path.string() + ".tmp"),
            "a nested configuration save should clean its temporary file");

        const auto external_config_path = user_dir / "linked-config.json";
        {
            std::ofstream external_stream(external_config_path);
            external_stream << R"json({
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": []
}
)json";
        }
        std::filesystem::remove(linked_config_path);
        std::filesystem::create_symlink(std::filesystem::path("..") / "linked-config.json", linked_config_path);

        MAA_PROJECT_INTERFACE_NS::Configurator rejected_configurator;
        require(rejected_configurator.load(resource_dir, user_dir), "the external symlink configuration fixture should load");
        rejected_configurator.configuration().resource = "other-resource";
        require(!rejected_configurator.save(user_dir), "saving should reject a configuration symlink outside the configuration directory");
        const auto external_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(external_config_path);
        require(
            external_config && external_config->resource == "default-resource",
            "a rejected configuration symlink should not replace its external target");

        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove_all(user_dir);
    }

    {
        const auto resource_dir = unique_temp_directory();
        const auto shared_user_dir = unique_temp_directory();
        const auto first_user_dir = unique_temp_directory();
        const auto second_user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);
        std::filesystem::create_directories(shared_user_dir / "config");
        std::filesystem::create_directories(first_user_dir);
        std::filesystem::create_directories(second_user_dir);
        std::filesystem::create_directory_symlink(shared_user_dir / "config", first_user_dir / "config");
        std::filesystem::create_directory_symlink(shared_user_dir / "config", second_user_dir / "config");

        {
            std::ofstream interface_stream(resource_dir / "interface.json");
            interface_stream << R"json({
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [
        { "name": "default-resource", "path": [ "resource" ] },
        { "name": "other-resource", "path": [ "resource" ] }
    ],
    "task": [ { "name": "local-task", "entry": "LocalTask" } ],
    "welcome": "Welcome update"
}
)json";
        }

        const auto shared_config_path = shared_user_dir / "config/maa_pi_config.json";
        {
            std::ofstream config_stream(shared_config_path);
            config_stream << R"json({
    "controller": { "name": "adb-controller" },
    "resource": "default-resource",
    "task": [],
    "last_welcome": [ "old-welcome" ],
    "last_resolved_welcome": [ "Old welcome" ]
}
)json";
        }

        MAA_PROJECT_INTERFACE_NS::Configurator first;
        MAA_PROJECT_INTERFACE_NS::Configurator second;
        require(first.load(resource_dir, first_user_dir), "the first aliased configuration fixture should load");
        require(second.load(resource_dir, second_user_dir), "the second aliased configuration fixture should load");

        first.configuration().resource = "other-resource";
        require(first.save(first_user_dir), "the first aliased local configuration edit should save");

        second.configuration().task.emplace_back(MAA_PROJECT_INTERFACE_NS::Configuration::Task { .name = "local-task" });
        require(second.save(second_user_dir), "the second aliased local configuration edit should save");
        const auto changed = second.update_welcome_snapshots(second_user_dir, { "Welcome update" }, { "Welcome update" });
        require(changed.has_value() && *changed, "the changed aliased welcome snapshot should save");

        const auto saved_config = MAA_PROJECT_INTERFACE_NS::Parser::parse_config(shared_config_path);
        require(
            saved_config && saved_config->resource == "other-resource" && saved_config->task.size() == 1
                && saved_config->task.front().name == "local-task",
            "aliased configuration edits should preserve each other");
        require(
            saved_config && saved_config->last_welcome == std::vector<std::string> { "Welcome update" }
                && saved_config->last_resolved_welcome == std::vector<std::string> { "Welcome update" },
            "aliased welcome snapshots should preserve newer configuration settings");
        require(
            std::filesystem::exists(shared_user_dir / "config/.maa_pi_config.lock")
                && std::filesystem::is_symlink(std::filesystem::symlink_status(first_user_dir / "config"))
                && std::filesystem::is_symlink(std::filesystem::symlink_status(second_user_dir / "config")),
            "aliased configuration saves should share one canonical lock");

        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove_all(shared_user_dir);
        std::filesystem::remove_all(first_user_dir);
        std::filesystem::remove_all(second_user_dir);
    }
#endif

    {
        const auto resource_dir = unique_temp_directory();
        const auto user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);

        {
            std::ofstream interface_stream(resource_dir / "interface.json");
            interface_stream << R"json({
    "interface_version": 2,
    "controller": [ { "name": "adb-controller", "type": "Adb" } ],
    "resource": [ { "name": "default-resource", "path": [ "resource" ] } ]
}
)json";
        }

        {
            std::ofstream user_path_stream(user_dir);
            user_path_stream << "not a directory";
        }

        MAA_PROJECT_INTERFACE_NS::Configurator configurator;
        require(configurator.load(resource_dir, user_dir), "a first-time configuration should load despite an invalid user directory");
        configurator.configuration().resource = "default-resource";

        bool saved = true;
        bool threw = false;
        try {
            saved = configurator.save(user_dir);
        }
        catch (const std::exception&) {
            threw = true;
        }
        require(!saved && !threw, "an inaccessible user directory should fail configuration saving without throwing");

        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove(user_dir);
    }

    {
        using namespace MAA_PROJECT_INTERFACE_NS;
        const auto resource_dir = unique_temp_directory();
        const auto ios_user_dir = unique_temp_directory();
        std::filesystem::create_directories(resource_dir);
        {
            std::ofstream stream(resource_dir / "interface.json");
            stream << R"json({
                "interface_version": 2,
                "controller": [{"name": "ios", "type": "IOS", "ios": {
                    "udid": "test-iphone", "connection": "WiFi", "screencap": "Stream",
                    "config": {"server_port": 17424, "wifi_address": "10.0.0.1"}
                }}],
                "resource": [{"name": "default", "path": ["resource"]}],
                "task": [{"name": "capture", "entry": "Capture"}]
            })json";
        }
        Configurator configurator;
        require(configurator.load(resource_dir, ios_user_dir), "iOS project configuration should load");
        auto& configuration = configurator.configuration();
        configuration.controller.name = "ios";
        configuration.resource = "default";
        configuration.task.push_back({ "capture", { } });
        require(configurator.check_configuration(), "iOS controller should be selected by name");
        configuration.ios.screencap = "ScreenshotService";
        const auto runtime = configurator.generate_runtime();
        const auto* ios = runtime ? std::get_if<RuntimeParam::IOSParam>(&runtime->controller_param) : nullptr;
        require(
            ios && ios->udid == "test-iphone" && ios->connection == MaaIOSConnectionMethod_WiFi,
            "iOS runtime should use project device defaults");
        require(ios && ios->screencap == MaaIOSScreencapMethod_ScreenshotService, "saved iOS screencap choice should override the project");
        require(ios && ios->config.find("17424") != std::string::npos, "iOS server configuration should reach the runtime");
        configuration.ios.config = { { "max_fps", 15 } };
        const auto merged_runtime = configurator.generate_runtime();
        const auto* merged = merged_runtime ? std::get_if<RuntimeParam::IOSParam>(&merged_runtime->controller_param) : nullptr;
        const auto merged_options = merged ? json::parse(merged->config) : std::nullopt;
        require(
            merged_options && merged_options->get("server_port", 0) == 17424 && merged_options->get("max_fps", 0) == 15,
            "user fields must merge with project server settings");
        configuration.ios.config = { };
        configuration.ios.connection = "USB";
        const auto usb_runtime = configurator.generate_runtime();
        const auto* usb = usb_runtime ? std::get_if<RuntimeParam::IOSParam>(&usb_runtime->controller_param) : nullptr;
        require(usb && usb->connection == MaaIOSConnectionMethod_USB, "saved USB connection should override project Wi-Fi");
        const auto usb_options = usb ? json::parse(usb->config) : std::nullopt;
        require(usb_options && !usb_options->contains("wifi_address"), "USB must ignore project Wi-Fi addresses");
        configuration.ios.connection = "Auto";
        const auto auto_runtime = configurator.generate_runtime();
        const auto* automatic = auto_runtime ? std::get_if<RuntimeParam::IOSParam>(&auto_runtime->controller_param) : nullptr;
        const auto auto_options = automatic ? json::parse(automatic->config) : std::nullopt;
        require(
            auto_options && auto_options->get("wifi_address", std::string()) == "10.0.0.1",
            "Auto must preserve an explicit project Wi-Fi address");
        for (const auto& address : { "10.0.0.99", "auto", "" }) {
            configuration.ios.config = { { "wifi_address", address }, { "max_fps", 15 } };
            const auto selected_runtime = configurator.generate_runtime();
            const auto* selected = selected_runtime ? std::get_if<RuntimeParam::IOSParam>(&selected_runtime->controller_param) : nullptr;
            const auto selected_options = selected ? json::parse(selected->config) : std::nullopt;
            require(
                selected && selected->connection == MaaIOSConnectionMethod_Auto && selected_options
                    && selected_options->get("wifi_address", std::string("missing")) == address
                    && selected_options->get("server_port", 0) == 17424 && selected_options->get("max_fps", 0) == 15,
                "Auto must preserve explicit, discovery and empty Wi-Fi addresses alongside unrelated options");
        }
        configuration.ios.config = { };
        configuration.ios.connection = "invalid";
        require(!configurator.generate_runtime(), "unknown iOS transport must fail runtime generation");
        configuration.ios.connection.clear();
        configuration.ios.udid = "saved-iphone";
        require(configurator.save(ios_user_dir), "iOS user configuration should save");
        {
            StreamRedirector redirector("1\n\n3\n10.0.0.99\n3\n7\n");
            Interactor interactor(ios_user_dir);
            require(interactor.load(resource_dir), "saved iOS configuration should load in the CLI");
            require(interactor.interact(), "iOS device and screencap selection should complete");
        }
        auto saved = Parser::parse_config(ios_user_dir / "config/maa_pi_config.json");
        require(
            saved && saved->ios.udid == "saved-iphone" && saved->ios.screencap == "Stream|ScreenshotService",
            "iOS selection should preserve the UDID and save the combined screencap method");
        require(saved && saved->ios.config.get("wifi_address", std::string()) == "10.0.0.99", "Wi-Fi selection should save its address");
        {
            StreamRedirector redirector("1\n\n1\n\n2\n7\n");
            Interactor interactor(ios_user_dir);
            require(interactor.load(resource_dir) && interactor.interact(), "switching from Wi-Fi to Auto should complete");
        }
        saved = Parser::parse_config(ios_user_dir / "config/maa_pi_config.json");
        require(
            saved && saved->ios.connection == "Auto" && saved->ios.config.get("wifi_address", std::string()) == "10.0.0.99"
                && saved->ios.config.get("server_port", 0) == 17424,
            "Auto must preserve a supplied Wi-Fi address and server settings");
        {
            StreamRedirector redirector("1\n\n1\n-\n2\n7\n");
            Interactor interactor(ios_user_dir);
            require(interactor.load(resource_dir) && interactor.interact(), "clearing the Auto Wi-Fi address should complete");
        }
        saved = Parser::parse_config(ios_user_dir / "config/maa_pi_config.json");
        require(
            saved && saved->ios.connection == "Auto" && saved->ios.config.get("wifi_address", std::string("missing")).empty()
                && saved->ios.config.get("server_port", 0) == 17424,
            "an explicitly empty address must override the project address without losing server settings");
        {
            StreamRedirector redirector("1\n");
            Interactor interactor(ios_user_dir);
            require(interactor.load(resource_dir), "iOS EOF fixture should load");
            require(interactor.interact(), "EOF during iOS configuration should exit cleanly");
            require(
                redirector.output().str().find("Choose screencap method") == std::string::npos,
                "EOF at the UDID prompt should not advance to screencap selection");
        }
        const auto after_eof = Parser::parse_config(ios_user_dir / "config/maa_pi_config.json");
        require(after_eof && saved && after_eof->ios == saved->ios, "aborted iOS selection should not overwrite saved configuration");
        Configurator first_writer, second_writer;
        require(
            first_writer.load(resource_dir, ios_user_dir) && second_writer.load(resource_dir, ios_user_dir),
            "concurrent iOS configurations should load");
        first_writer.configuration().ios.udid = "first-writer";
        first_writer.configuration().ios.config["max_fps"] = 10;
        second_writer.configuration().ios.screencap = "Stream";
        second_writer.configuration().ios.config["hardware_decode"] = false;
        require(first_writer.save(ios_user_dir) && second_writer.save(ios_user_dir), "independent iOS edits should save");
        const auto combined = Parser::parse_config(ios_user_dir / "config/maa_pi_config.json");
        require(
            combined && combined->ios.udid == "first-writer" && combined->ios.screencap == "Stream"
                && combined->ios.config.get("max_fps", 0) == 10 && combined->ios.config.contains("hardware_decode"),
            "concurrent writers must preserve independent iOS fields and config keys");

        std::filesystem::create_directories(resource_dir);
        {
            std::ofstream stream(resource_dir / "interface.json");
            stream << R"json({
                "interface_version": 2,
                "controller": [
                    {"name": "ios", "type": "IOS", "ios": {"udid": "phone-a"}},
                    {"name": "second", "type": "IOS", "ios": {
                        "udid": "phone-b", "connection": "USB", "screencap": "ScreenshotService",
                        "config": {"server_port": 17425}
                    }}
                ],
                "resource": [{"name": "default", "path": ["resource"]}],
                "task": [{"name": "capture", "entry": "Capture"}]
            })json";
        }
        {
            StreamRedirector redirector("1\n2\n\n\n\n7\n");
            Interactor interactor(ios_user_dir);
            require(interactor.load(resource_dir) && interactor.interact(), "switching iOS controllers should complete");
        }
        const auto second = Parser::parse_config(ios_user_dir / "config/maa_pi_config.json");
        require(
            second && second->controller.name == "second" && second->ios.udid == "phone-b" && second->ios.connection == "USB"
                && second->ios.screencap == "ScreenshotService" && second->ios.config.get("server_port", 0) == 17425,
            "new controller must use its own device, settings and default menu choices");
        std::filesystem::remove_all(resource_dir);
        std::filesystem::remove_all(ios_user_dir);
    }

    if (failures != 0) {
        std::cerr << failures << " interactor test assertion(s) failed\n";
        return 1;
    }

    std::cout << "MaaPiCli interactor tests passed\n";
    return 0;
}
