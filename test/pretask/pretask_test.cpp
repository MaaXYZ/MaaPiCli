#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <meojson/json.hpp>

#include "ProjectInterface/Configurator.h"
#include "ProjectInterface/Parser.h"
#include "ProjectInterface/Runner.h"

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

std::vector<std::string> names(const std::vector<MAA_PROJECT_INTERFACE_NS::RuntimeParam::Pretask>& pretasks)
{
    std::vector<std::string> result;
    result.reserve(pretasks.size());
    for (const auto& pretask : pretasks) {
        result.emplace_back(pretask.name);
    }
    return result;
}
} // namespace

int main()
{
    using namespace MAA_PROJECT_INTERFACE_NS;

    const std::filesystem::path fixture_dir = MAAPICLI_TEST_FIXTURE_DIR;
    const std::filesystem::path runtime_fixture_dir = fixture_dir / "pretask_runtime";

    require(!Parser::parse_interface(fixture_dir / "invalid_pretask_exec.json").has_value(), "an empty pretask exec should be rejected");

    Configurator configurator;
    require(configurator.load(runtime_fixture_dir, fixture_dir / "pretask-user"), "runtime fixture should load");

    auto& config = configurator.configuration();
    config.controller.name = "default-controller";
    config.controller.type = InterfaceData::Controller::Type::Adb;
    config.resource = "default-resource";
    config.task.emplace_back(Configuration::Task { .name = "main-task" });

    Configuration::Option policy;
    policy.name = "policy";
    policy.value = "fast";

    Configuration::Option flags;
    flags.name = "flags";
    flags.values = { "alpha", "beta" };

    Configuration::Option credentials;
    credentials.name = "credentials";
    credentials.inputs["token"] = "secret-token";
    credentials.inputs["file"] = "C:\\future folder\\config.json";
    credentials.inputs["directory"] = "/future folder/output";

    const auto& credentials_inputs = configurator.interface_data().option.at("credentials").inputs;
    require(
        std::ranges::any_of(credentials_inputs, [](const auto& input) { return input.name == "token" && input.password; }),
        "pretask fixture should parse the password flag");

    Configuration::Pretask pretask_config;
    pretask_config.name = "ordered-first";
    pretask_config.option = { std::move(policy), std::move(flags), std::move(credentials) };
    config.pretask.emplace_back(std::move(pretask_config));

    auto runtime = configurator.generate_runtime();
    require(runtime.has_value(), "runtime should generate");
    if (runtime) {
        const auto actual_names = names(runtime->pretask);
        const std::vector<std::string> expected_names = { "ordered-first", "ordered-second" };
        require(actual_names == expected_names, "pretasks should be filtered and retain merged order");

        if (runtime->pretask.size() == 2) {
            const auto& first = runtime->pretask.front();
            require(first.args.size() == 3, "fixed args and one option JSON argument should be present");
            require(first.args[0] == "before" && first.args[1] == "--mode", "fixed args should remain in order");

            if (first.args.size() == 3) {
                auto options = json::parse(first.args[2]);
                require(options.has_value() && options->is_object(), "pretask options should be a JSON object");
                if (options && options->is_object()) {
                    require(options->at("policy") == "fast", "select options should serialize as strings");
                    require(options->at("flags") == json::array { "alpha", "beta" }, "checkbox options should serialize as string arrays");
                    require(
                        options->at("credentials")
                            == json::object { { "token", "secret-token" },
                                              { "file", "C:\\future folder\\config.json" },
                                              { "directory", "/future folder/output" } },
                        "password and path inputs should serialize as unchanged strings in pretask field objects");
                    require(!options->contains("hidden-option"), "inapplicable options should be omitted");
                }
            }

            const auto& second = runtime->pretask.back();
            require(second.exec == "second-pretask", "pretask exec should be preserved");
            require(second.cwd == runtime_fixture_dir, "pretask cwd should be the interface directory");
            require(second.args.empty(), "pretasks without options should not receive a JSON argument");
        }
    }

#if !defined(_WIN32)
    require(
        Runner::run_pretasks(
            { RuntimeParam::Pretask { .name = "cwd-check",
                                      .exec = "/bin/sh",
                                      .args = { "-c", "test -f interface.json" },
                                      .cwd = runtime_fixture_dir } }),
        "pretask should run with the interface directory as cwd");

    require(
        !Runner::run_pretasks(
            { RuntimeParam::Pretask { .name = "failure-check",
                                      .exec = "/bin/sh",
                                      .args = { "-c", "exit 3" },
                                      .cwd = runtime_fixture_dir } }),
        "a non-zero pretask exit code should fail");

    require(
        Runner::run_pretasks({ RuntimeParam::Pretask { .name = "path-check", .exec = "true", .args = { }, .cwd = runtime_fixture_dir } }),
        "pretasks should support executables in PATH");
#endif

    if (failures != 0) {
        std::cerr << failures << " pretask test assertion(s) failed\n";
        return 1;
    }

    std::cout << "MaaPiCli pretask tests passed\n";
    return 0;
}
