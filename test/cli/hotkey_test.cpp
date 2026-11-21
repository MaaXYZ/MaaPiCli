#include "CLI/interactor.h"
#include "Impl/TerminalText.h"
#include "ProjectInterface/Parser.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace
{
int failures = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void check_preset(
    bool pretask,
    const json::value& value,
    const std::string& default_combo,
    bool accepted,
    const std::string& controller = "Adb",
    const std::string& expected_combo = "Ctrl+E")
{
    using namespace MAA_PROJECT_INTERFACE_NS;
    const auto root = std::filesystem::temp_directory_path()
                      / ("maapicli-hotkey-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "user/config");
    auto interface = *json::parse(R"({
        "interface_version": 2,
        "controller": [{"name":"controller", "type":"Adb"}],
        "resource": [{"name":"resource", "path":["resource"]}],
        "task": [{"name":"original", "entry":"Original"}, {"name":"task", "entry":"Main", "option":["shortcut"]}],
        "option": {"shortcut":{"type":"hotkey", "hotkeys":[{"name":"combo", "default":"Ctrl+E"}]}},
        "pretask": [{"name":"prepare", "exec":"unused", "option":["shortcut"]}],
        "preset": [{"name":"preset", "task":[{"name":"task"}]}]
    })");
    interface["option"]["shortcut"]["hotkeys"][0]["default"] = default_combo;
    interface["controller"][0]["type"] = controller;
    if (pretask) {
        interface["preset"][0]["task"] = json::array { json::object { { "name", "original" } } };
        interface["preset"][0]["pretask"] = json::array { json::object { { "name", "prepare" } } };
    }
    if (!value.is_null()) {
        interface["preset"][0][pretask ? "pretask" : "task"][0]["option"] = json::object { { "shortcut", value } };
    }
    {
        std::ofstream out(root / "interface.json");
        out << interface.dumps();
    }
    Configuration config;
    config.controller.name = "controller";
    config.resource = "resource";
    config.enabled_pretask = std::vector<std::string> { };
    config.task.emplace_back().name = "original";
    {
        std::ofstream out(root / "user/config/maa_pi_config.json");
        out << config.to_json().dumps();
    }
    std::istringstream input("7\n1\n8\n");
    std::ostringstream output;
    const auto old_input = std::cin.rdbuf(input.rdbuf());
    const auto old_output = std::cout.rdbuf(output.rdbuf());
    {
        Interactor interactor(root / "user");
        require(interactor.load(root), "hotkey fixture should load");
        require(interactor.interact(), "preset interaction should exit cleanly");
    }
    std::cin.rdbuf(old_input);
    std::cout.rdbuf(old_output);
    require((output.str().find("Applied preset:") != std::string::npos) == accepted, "preset acceptance should match hotkey validity");
    const auto saved = Parser::parse_config(root / "user/config/maa_pi_config.json");
    require(saved.has_value(), "configuration should remain readable");
    if (saved && accepted) {
        const auto& options = pretask ? saved->pretask.front().option : saved->task.front().option;
        require(options.size() == 1 && options.front().hotkeys.at("combo") == expected_combo, "preset strings should be canonicalized");
    }
    if (saved && !accepted) {
        require(
            saved->task.size() == 1 && saved->task.front().name == "original" && saved->pretask.empty(),
            "invalid presets must preserve the existing configuration");
    }
    std::filesystem::remove_all(root);
}

void check_failed_completion(const std::string& scope)
{
    using namespace MAA_PROJECT_INTERFACE_NS;
    const auto root = std::filesystem::temp_directory_path()
                      / ("maapicli-hotkey-failure-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "user/config");
    auto interface = *json::parse(R"({
        "interface_version": 2,
        "controller": [{"name":"controller", "type":"Adb"}],
        "resource": [{"name":"resource", "path":["resource"]}],
        "task": [{"name":"task", "entry":"Main"}],
        "option": {
            "first":{"type":"input", "inputs":[{"name":"note"}, {"name":"missing", "default":"new"}]},
            "shortcut":{"type":"hotkey", "hotkeys":[{"name":"combo", "default":"Ctrl+E"}]},
            "last":{"type":"input", "inputs":[{"name":"note"}]}
        }
    })");
    const json::array declarations { "first", "shortcut", "last" };
    if (scope == "task") {
        interface["task"][0]["option"] = declarations;
    }
    else if (scope == "global") {
        interface["global_option"] = declarations;
    }
    else {
        interface["pretask"] = json::array { json::object { { "name", "prepare" }, { "exec", "unused" }, { "option", declarations } } };
    }
    {
        std::ofstream out(root / "interface.json");
        out << interface.dumps();
    }
    Configuration config;
    config.controller.name = "controller";
    config.resource = "resource";
    config.task.emplace_back().name = "task";
    config.enabled_pretask = scope == "pretask" ? std::vector<std::string> { "prepare" } : std::vector<std::string> { };
    Configuration::Option first;
    first.name = "first";
    first.inputs["note"] = "keep-first";
    Configuration::Option shortcut;
    shortcut.name = "shortcut";
    shortcut.hotkeys["combo"] = "NoSuchKey";
    Configuration::Option last;
    last.name = "last";
    last.inputs["note"] = "keep-last";
    const std::vector<Configuration::Option> original { first, shortcut, last };
    if (scope == "task") {
        config.task.front().option = original;
    }
    else if (scope == "global") {
        config.global_option = original;
    }
    else {
        config.pretask.emplace_back(Configuration::Pretask { .name = "prepare", .option = original });
    }
    {
        std::ofstream out(root / "user/config/maa_pi_config.json");
        out << config.to_json().dumps();
    }
    // Moving the only task to its current position performs a later successful
    // action and save, exposing any partial mutation left by the failed run.
    std::istringstream input("4\n1\n1\n7\n");
    std::ostringstream output;
    const auto old_input = std::cin.rdbuf(input.rdbuf());
    const auto old_output = std::cout.rdbuf(output.rdbuf());
    {
        Interactor interactor(root / "user");
        require(interactor.load(root), "invalid saved hotkey fixture should load");
        require(!interactor.run(), "invalid saved hotkeys must stop before executing tasks");
        require(interactor.interact(), "a subsequent action should save and exit cleanly");
    }
    std::cin.rdbuf(old_input);
    std::cout.rdbuf(old_output);
    const auto saved = Parser::parse_config(root / "user/config/maa_pi_config.json");
    require(saved.has_value(), "configuration should remain readable after failure and subsequent save");
    if (saved) {
        const auto& options = scope == "task"     ? saved->task.front().option
                              : scope == "global" ? saved->global_option
                                                  : saved->pretask.front().option;
        require(options == original, "failed completion must preserve every original option and field through a later save");
    }
    std::filesystem::remove_all(root);
}

void check_hotkey_option_names()
{
    using namespace MAA_PROJECT_INTERFACE_NS;
    const auto root = std::filesystem::temp_directory_path()
                      / ("maapicli-hotkey-name-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "user/config");
    const std::string unsafe_name =
        "shortcut\x1b[2J\xc2\x9b"
        "31m-safe";
    const std::string clean_name = "shortcut-safe";
    auto interface = *json::parse(R"({
        "interface_version": 2,
        "controller": [{"name":"controller", "type":"Adb"}],
        "resource": [{"name":"resource", "path":["resource"]}],
        "task": [{"name":"task", "entry":"Main"}],
        "option": {}
    })");
    interface["global_option"] = json::array { unsafe_name };
    interface["task"][0]["option"] = json::array { unsafe_name };
    interface["option"][unsafe_name] =
        json::object { { "type", "hotkey" }, { "hotkeys", json::array { json::object { { "name", "combo" }, { "default", "Ctrl+E" } } } } };
    {
        std::ofstream out(root / "interface.json");
        out << interface.dumps();
    }
    Configuration config;
    config.controller.name = "controller";
    config.resource = "resource";
    Configuration::Option option;
    option.name = unsafe_name;
    option.hotkeys["combo"] = "Ctrl+E";
    config.global_option = { option };
    config.task.emplace_back(Configuration::Task { .name = "task", .option = { option } });
    {
        std::ofstream out(root / "user/config/maa_pi_config.json");
        out << config.to_json().dumps();
    }
    std::istringstream input;
    std::ostringstream output;
    const auto old_input = std::cin.rdbuf(input.rdbuf());
    const auto old_output = std::cout.rdbuf(output.rdbuf());
    {
        Interactor interactor(root / "user");
        require(interactor.load(root), "hotkey option name fixture should load");
        interactor.print_config();
    }
    std::cin.rdbuf(old_input);
    std::cout.rdbuf(old_output);
    const auto displayed = output.str();
    require(displayed.find(unsafe_name) == std::string::npos, "raw option name controls must not reach either configuration view");
    require(displayed.find("\t" + clean_name + ":") != std::string::npos, "global option names should be sanitized");
    require(displayed.find("\t\t- " + clean_name + ":") != std::string::npos, "task option names should be sanitized");
    const auto saved = Parser::parse_config(root / "user/config/maa_pi_config.json");
    require(
        saved && saved->global_option.front().name == unsafe_name && saved->task.front().option.front().name == unsafe_name,
        "display sanitization must not change stored option identifiers");
    std::filesystem::remove_all(root);
}

}

int main()
{
    require(MaaPiCliTerminal::sanitize("Ctrl+\x1b[2JA") == "Ctrl+A", "CSI controls should be removed with their payload");
    require(MaaPiCliTerminal::sanitize("\x1b]52;c;secret\aA") == "A", "OSC clipboard commands should be removed");
    require(MaaPiCliTerminal::sanitize("\x1b]title\x1b\\A") == "A", "ST-terminated OSC should be removed");
    require(MaaPiCliTerminal::sanitize("A\n\tB\r\b") == "AB", "single-line hotkey text should not inject formatting");
    require(MaaPiCliTerminal::sanitize("快捷键") == "快捷键", "UTF-8 text should be preserved");
    require(
        MaaPiCliTerminal::sanitize("第一行\n\t\x1b[31m第二行\x1b[0m\r\b", true) == "第一行\n\t第二行",
        "multiline focus messages should retain line breaks and tabs while removing controls");
    for (unsigned int control = 0x80; control <= 0x9f; ++control) {
        const std::string encoded { static_cast<char>(0xc2), static_cast<char>(control) };
        require(MaaPiCliTerminal::sanitize(encoded).empty(), "all Unicode C1 controls should be removed from single-line fields");
        require(MaaPiCliTerminal::sanitize(encoded, true).empty(), "all Unicode C1 controls should be removed from multiline messages");
    }
    require(
        MaaPiCliTerminal::sanitize(
            "A\xc2\x9b"
            "2JB")
            == "AB",
        "Unicode CSI should be removed with its command payload");
    require(
        MaaPiCliTerminal::sanitize(
            "\xc2\x9d"
            "52;c;secret\xc2\x9c"
            "A")
            == "A",
        "Unicode OSC and ST should remove clipboard payloads");
    require(
        MaaPiCliTerminal::sanitize(
            "\x1b]title\xc2\x9c"
            "A")
            == "A",
        "ESC OSC should accept a Unicode ST terminator");
    require(
        MaaPiCliTerminal::sanitize(
            "\xc2\x9d"
            "title\x1b\\A")
            == "A",
        "Unicode OSC should accept an ESC ST terminator");
    require(
        MaaPiCliTerminal::sanitize(
            "A\xc2\x85"
            "B\n\tC",
            true)
            == "AB\n\tC",
        "C1 removal should retain allowed ASCII formatting");
    require(MaaPiCliTerminal::sanitize("界面🙂é") == "界面🙂é", "UTF-8 continuation bytes in the C1 range must remain intact");
    require(
        MaaPiCliTerminal::sanitize(
            "A\x9b"
            "B")
            == "AB",
        "standalone invalid C1 bytes should not reach the terminal");
    require(
        MaaPiCliTerminal::sanitize(
            "A\xe0\x82\x9b"
            "B")
            == "AB",
        "overlong UTF-8 controls should not reach the terminal");
    check_hotkey_option_names();
    for (const auto& scope : { "task", "global", "pretask" }) {
        check_failed_completion(scope);
    }
    for (bool pretask : { false, true }) {
        check_preset(pretask, json::object { { "combo", "control+f" } }, "Ctrl+E", true, "Adb", "Ctrl+F");
        check_preset(pretask, json::object { { "combo", "NoSuchKey" } }, "Ctrl+E", false);
        check_preset(pretask, json::value(), "NoSuchKey", false);
        check_preset(pretask, json::object { }, "NoSuchKey", false);
        check_preset(pretask, json::value(), "Ctrl+E", true);
        check_preset(pretask, "Ctrl+E", "Ctrl+E", false);
        check_preset(pretask, json::object { { "combo", 42 } }, "Ctrl+E", false);
        check_preset(pretask, json::object { { "unknown", "Ctrl+E" } }, "Ctrl+E", false);
        check_preset(pretask, json::object { { "combo", "Insert" } }, "Ctrl+E", false, "MacOS");
    }
    return failures == 0 ? 0 : 1;
}
