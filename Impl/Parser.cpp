#include "ProjectInterface/Parser.h"

#include <cstdint>
#include <functional>
#include <ranges>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "MaaUtils/Logger.h"

MAA_PROJECT_INTERFACE_NS_BEGIN

namespace
{
void normalize_legacy_wlroots(json::value& json)
{
    if (!json.is_object()) {
        return;
    }

    auto rename_legacy_controller_key = [](json::value& controller) {
        if (!controller.is_object()) {
            return;
        }

        if (controller.contains("wlroots") && !controller.contains("linux")) {
            controller["linux"] = controller["wlroots"];
        }
        controller.erase("wlroots");

        if (controller.contains("type") && controller["type"].is_string() && controller["type"].as_string() == "WlRoots") {
            controller["type"] = "Linux";
        }
    };

    if (json.contains("controller") && json["controller"].is_array()) {
        for (auto& controller : json["controller"].as_array()) {
            rename_legacy_controller_key(controller);
        }
    }

    if (json.contains("wlroots")) {
        if (!json.contains("linux")) {
            json["linux"] = json["wlroots"];
        }
        json.erase("wlroots");
    }
}

std::optional<InterfaceData> deserialize_interface(const json::value& json)
{
    std::string error_key;
    if (!InterfaceData().check_json(json, error_key)) {
        LogError << "json is not an InterfaceData" << VAR(error_key) << VAR(json);
        return std::nullopt;
    }

    return json.as<InterfaceData>();
}

std::vector<std::string> unique_values(const std::vector<std::string>& values)
{
    std::unordered_set<std::string> seen;
    std::vector<std::string> unique;
    for (const auto& value : values) {
        if (seen.insert(value).second) {
            unique.emplace_back(value);
        }
    }
    return unique;
}

size_t valid_unique_checkbox_count(const InterfaceData::Option& option, const std::vector<std::string>& values)
{
    std::unordered_set<std::string> seen;
    for (const auto& value : values) {
        if (std::ranges::find(option.cases, value, std::mem_fn(&InterfaceData::Option::Case::name)) != option.cases.end()) {
            seen.insert(value);
        }
    }
    return seen.size();
}

bool checkbox_selection_is_valid(const InterfaceData::Option& option, const std::vector<std::string>& values)
{
    const auto selection_count = valid_unique_checkbox_count(option, values);
    return (!option.min_count || selection_count >= *option.min_count) && (!option.max_count || selection_count <= *option.max_count);
}

bool option_is_applicable(const InterfaceData::Option& option, const Configuration& config)
{
    if (!option.controller.empty() && std::ranges::find(option.controller, config.controller.name) == option.controller.end()) {
        return false;
    }
    if (!option.resource.empty() && std::ranges::find(option.resource, config.resource) == option.resource.end()) {
        return false;
    }
    return true;
}

bool task_is_applicable(const InterfaceData::Task& task, const Configuration& config)
{
    if (!task.controller.empty() && std::ranges::find(task.controller, config.controller.name) == task.controller.end()) {
        return false;
    }
    if (!task.resource.empty() && std::ranges::find(task.resource, config.resource) == task.resource.end()) {
        return false;
    }
    return true;
}

bool validate_task_option_selection(
    const InterfaceData::Option& data_option,
    Configuration::Option& config_option,
    std::vector<const InterfaceData::Option::Case*>& selected_cases,
    bool& changed)
{
    switch (data_option.type) {
    case InterfaceData::Option::Type::Select:
    case InterfaceData::Option::Type::Switch: {
        auto case_iter = std::ranges::find(data_option.cases, config_option.value, std::mem_fn(&InterfaceData::Option::Case::name));
        if (case_iter == data_option.cases.end()) {
            return false;
        }
        selected_cases.emplace_back(&*case_iter);
    } break;

    case InterfaceData::Option::Type::Checkbox: {
        auto deduped_values = unique_values(config_option.values);
        if (deduped_values.size() != config_option.values.size()) {
            config_option.values = std::move(deduped_values);
            changed = true;
        }

        if (!checkbox_selection_is_valid(data_option, config_option.values)) {
            return false;
        }

        for (const auto& value : config_option.values) {
            auto case_iter = std::ranges::find(data_option.cases, value, std::mem_fn(&InterfaceData::Option::Case::name));
            if (case_iter == data_option.cases.end()) {
                return false;
            }
            selected_cases.emplace_back(&*case_iter);
        }
    } break;

    case InterfaceData::Option::Type::Input:
        break;
    }

    return true;
}

bool match_task_option_subtree(
    const InterfaceData& data,
    const Configuration& config,
    const std::string& option_name,
    std::vector<Configuration::Option>::iterator& config_option,
    const std::vector<Configuration::Option>::iterator& end,
    bool& changed)
{
    if (config_option == end || config_option->name != option_name) {
        return false;
    }

    auto data_option_iter = data.option.find(option_name);
    if (data_option_iter == data.option.end() || !option_is_applicable(data_option_iter->second, config)) {
        return false;
    }

    std::vector<const InterfaceData::Option::Case*> selected_cases;
    if (!validate_task_option_selection(data_option_iter->second, *config_option, selected_cases, changed)) {
        return false;
    }

    ++config_option;
    for (const auto* selected_case : selected_cases) {
        for (const auto& child_name : selected_case->option) {
            auto child_iter = data.option.find(child_name);
            if (child_iter == data.option.end()) {
                return false;
            }

            // process_option stores only options applicable to the active controller and resource.
            if (!option_is_applicable(child_iter->second, config)) {
                continue;
            }

            if (!match_task_option_subtree(data, config, child_name, config_option, end, changed)) {
                return false;
            }
        }
    }

    return true;
}

bool validate_checkbox_definition(const InterfaceData::Option& option)
{
    if (option.min_count && *option.min_count > option.cases.size()) {
        return false;
    }
    if (option.max_count && *option.max_count > option.cases.size()) {
        return false;
    }
    if (option.min_count && option.max_count && *option.min_count > *option.max_count) {
        return false;
    }

    if (auto* defaults = std::get_if<std::vector<std::string>>(&option.default_case)) {
        for (const auto& value : *defaults) {
            if (std::ranges::find(option.cases, value, std::mem_fn(&InterfaceData::Option::Case::name)) == option.cases.end()) {
                return false;
            }
        }
        return checkbox_selection_is_valid(option, *defaults);
    }

    return true;
}

bool validate_display_options(const InterfaceData::Controller& controller)
{
    if (controller.display_expand.has_value() && ((*controller.display_expand)[0] <= 0 || (*controller.display_expand)[1] <= 0)) {
        LogError << "Display expand dimensions must be positive" << VAR(controller.name);
        return false;
    }

    const auto selected_count = (controller.display_short_side.has_value() ? 1 : 0) + (controller.display_long_side.has_value() ? 1 : 0)
                                + (controller.display_expand.has_value() ? 1 : 0) + (controller.display_raw ? 1 : 0);
    if (selected_count <= 1) {
        return true;
    }

    LogError << "Display options are mutually exclusive" << VAR(controller.name);
    return false;
}

bool validate_linux_options(const InterfaceData::Controller& controller)
{
    if (!controller.lnx.screencap.empty() && controller.lnx.screencap != "Wlr" && controller.lnx.screencap != "PipeWire") {
        LogError << "Invalid Linux screencap method, expected Wlr or PipeWire" << VAR(controller.lnx.screencap);
        return false;
    }

    if (!controller.lnx.input.empty() && controller.lnx.input != "Wlr" && controller.lnx.input != "UInput"
        && controller.lnx.input != "Libei") {
        LogError << "Invalid Linux input method, expected Wlr, UInput, or Libei" << VAR(controller.lnx.input);
        return false;
    }

    if (controller.lnx.screencap == "PipeWire") {
        const auto& pipewire_source = controller.lnx.pipewire_source;
        if (pipewire_source != "Gamescope" && pipewire_source != "Portal") {
            LogError << "Invalid PipeWire source, expected Gamescope or Portal" << VAR(pipewire_source);
            return false;
        }
    }

    return true;
}

bool validate_interface(const InterfaceData& data)
{
    // check interface version
    if (data.interface_version != 2) {
        LogError << "Unsupported interface version, expected 2" << VAR(data.interface_version);
        return false;
    }

    auto check_option_refs = [&](const std::vector<std::string>& options) {
        for (const auto& option : options) {
            if (!data.option.contains(option)) {
                LogError << "Option not found" << VAR(option);
                return false;
            }
        }
        return true;
    };

    // check option and group for task
    for (const auto& task : data.task) {
        if (!check_option_refs(task.option)) {
            return false;
        }

        for (const auto& group : task.group) {
            auto group_iter = std::ranges::find(data.group, group, std::mem_fn(&InterfaceData::Group::name));
            if (group_iter == data.group.end()) {
                LogError << "Group not found" << VAR(group);
                return false;
            }
        }
    }

    if (!check_option_refs(data.global_option)) {
        return false;
    }

    for (const auto& setting : data.setting) {
        if (!check_option_refs(setting.option)) {
            return false;
        }
    }

    for (const auto& pretask : Parser::flatten_pretask(data.pretask)) {
        if (pretask.exec.empty()) {
            LogError << "Pretask exec is empty";
            return false;
        }
        if (!check_option_refs(pretask.option)) {
            return false;
        }
    }

    for (const auto& [name, option] : data.option) {
        if (!validate_checkbox_definition(option)) {
            LogError << "Invalid checkbox count constraint" << VAR(name);
            return false;
        }
    }

    std::unordered_map<std::string, uint8_t> visit_states;
    std::function<bool(const std::string&)> detect_option_cycle = [&](const std::string& option_name) -> bool {
        auto& state = visit_states[option_name];
        if (state == 1) {
            LogError << "Cyclic option reference detected" << VAR(option_name);
            return false;
        }
        if (state == 2) {
            return true;
        }

        state = 1;
        auto option_iter = data.option.find(option_name);
        if (option_iter == data.option.end()) {
            LogError << "Option not found" << VAR(option_name);
            return false;
        }

        for (const auto& data_case : option_iter->second.cases) {
            for (const auto& child_name : data_case.option) {
                if (!detect_option_cycle(child_name)) {
                    return false;
                }
            }
        }

        state = 2;
        return true;
    };

    for (const auto& [name, option] : data.option) {
        auto state_iter = visit_states.find(name);
        if ((state_iter == visit_states.end() || state_iter->second == 0) && !detect_option_cycle(name)) {
            return false;
        }
    }

    // check controller type
    for (const auto& ctrl : data.controller) {
        if (ctrl.type == InterfaceData::Controller::Type::Invalid) {
            LogError << "Invalid Controller Type" << VAR(ctrl.type);
            return false;
        }
        if (!validate_display_options(ctrl)) {
            return false;
        }
        if (ctrl.type == InterfaceData::Controller::Type::Linux && !validate_linux_options(ctrl)) {
            return false;
        }
    }

    LogInfo << "Interface Version:" << VAR(data.version);
    return true;
}
} // namespace

std::vector<InterfaceData::Pretask>
    Parser::flatten_pretask(const std::optional<std::variant<InterfaceData::Pretask, std::vector<InterfaceData::Pretask>>>& pretask)
{
    if (!pretask) {
        return { };
    }

    return std::visit(
        [](const auto& value) -> std::vector<InterfaceData::Pretask> {
            using value_t = std::decay_t<decltype(value)>;

            if constexpr (std::is_same_v<value_t, InterfaceData::Pretask>) {
                return { value };
            }
            else {
                return value;
            }
        },
        *pretask);
}

std::optional<InterfaceData> Parser::parse_interface(const std::filesystem::path& path)
{
    LogFunc << VAR(path);

    auto json_opt = json::open(path, true, true);
    if (!json_opt) {
        LogError << "failed to parse" << path;
        return std::nullopt;
    }

    const json::value& json = *json_opt;
    json::value normalized_json = json;
    normalize_legacy_wlroots(normalized_json);
    auto data_opt = deserialize_interface(normalized_json);
    if (!data_opt) {
        return std::nullopt;
    }

    InterfaceData& data = *data_opt;
    std::vector<InterfaceData::Pretask> merged_pretask = Parser::flatten_pretask(data.pretask);
    bool has_pretask = data.pretask.has_value();
    std::unordered_set<std::string> group_names;
    for (const auto& group : data.group) {
        group_names.insert(group.name);
    }

    std::unordered_set<std::string> global_option_names;
    for (const auto& option : data.global_option) {
        global_option_names.insert(option);
    }

    auto base_dir = path.parent_path();
    for (const std::string& import_path : data_opt->import_) {
        auto import_full_path = base_dir / MaaNS::path(import_path);
        auto import_data = parse_import_data(import_full_path);
        if (!import_data) {
            LogError << "failed to parse import interface data" << VAR(import_full_path) << VAR(import_path);
            return std::nullopt;
        }
        data.task.insert(
            data.task.end(),
            std::make_move_iterator(import_data->task.begin()),
            std::make_move_iterator(import_data->task.end()));

        for (auto& [name, option] : import_data->option) {
            data.option.insert_or_assign(name, std::move(option));
        }

        for (auto& option : import_data->global_option) {
            if (global_option_names.insert(option).second) {
                data.global_option.push_back(std::move(option));
            }
        }

        for (auto& group : import_data->group) {
            if (group_names.insert(group.name).second) {
                data.group.push_back(std::move(group));
            }
        }

        auto import_pretasks = Parser::flatten_pretask(import_data->pretask);
        has_pretask = has_pretask || import_data->pretask.has_value();
        merged_pretask.insert(
            merged_pretask.end(),
            std::make_move_iterator(import_pretasks.begin()),
            std::make_move_iterator(import_pretasks.end()));

        data.preset.insert(
            data.preset.end(),
            std::make_move_iterator(import_data->preset.begin()),
            std::make_move_iterator(import_data->preset.end()));

        data.setting.insert(
            data.setting.end(),
            std::make_move_iterator(import_data->setting.begin()),
            std::make_move_iterator(import_data->setting.end()));
    }

    if (has_pretask) {
        data.pretask = std::move(merged_pretask);
    }

    if (!validate_interface(data)) {
        return std::nullopt;
    }

    return data;
}

std::optional<InterfaceData> Parser::parse_interface(const json::value& json)
{
    json::value normalized_json = json;
    normalize_legacy_wlroots(normalized_json);
    auto data = deserialize_interface(normalized_json);
    if (!data) {
        return std::nullopt;
    }

    if (!validate_interface(*data)) {
        return std::nullopt;
    }

    return data;
}

std::optional<Configuration> Parser::parse_config(const std::filesystem::path& path)
{
    LogFunc << VAR(path);

    auto json_opt = json::open(path, true, true);
    if (!json_opt) {
        LogWarn << "failed to parse" << path;
        return std::nullopt;
    }

    const json::value& json = *json_opt;
    return parse_config(json);
}

std::optional<Configuration> Parser::parse_config(const json::value& json)
{
    LogFunc << VAR(json);

    json::value normalized_json = json;
    normalize_legacy_wlroots(normalized_json);

    std::string error_key;
    if (!Configuration().check_json(normalized_json, error_key)) {
        LogError << "json is not a Configuration" << VAR(error_key) << VAR(json);
        return std::nullopt;
    }

    return normalized_json.as<Configuration>();
}

std::optional<ImportData> Parser::parse_import_data(const std::filesystem::path& path)
{
    LogFunc << VAR(path);
    auto json_opt = json::open(path, true, true);
    if (!json_opt) {
        LogError << "failed to parse import interface" << VAR(path);
        return std::nullopt;
    }

    const json::value& json = *json_opt;
    return parse_import_data(json);
}

std::optional<ImportData> Parser::parse_import_data(const json::value& json)
{
    std::string error_key;
    if (!ImportData().check_json(json, error_key)) {
        LogError << "json is not a valid ImportData" << VAR(error_key);
        return std::nullopt;
    }

    return json.as<ImportData>();
}

bool Parser::check_configuration(const InterfaceData& data, Configuration& config)
{
    bool erased = false;

    for (auto iter = config.task.begin(); iter != config.task.end();) {
        bool task_changed = false;
        bool checked = check_task(data, config, *iter, task_changed);
        if (checked) {
            ++iter;
            erased = erased || task_changed;
        }
        else {
            iter = config.task.erase(iter);
            erased = true;
        }
    }

    auto resource_iter = std::ranges::find(data.resource, config.resource, std::mem_fn(&InterfaceData::Resource::name));
    if (resource_iter == data.resource.end()) {
        LogWarn << "Resource not found" << VAR(config.resource);
        config.resource.clear();
        return false;
    }

    auto controller_iter = std::ranges::find(data.controller, config.controller.name, std::mem_fn(&InterfaceData::Controller::name));
    if (controller_iter == data.controller.end()) {
        LogWarn << "Controller not found" << VAR(config.controller.name);
        config.controller.name.clear();
        return false;
    }
    config.controller.type = controller_iter->type;

    auto check_option_list = [&](std::vector<Configuration::Option>& opts) {
        for (auto it = opts.begin(); it != opts.end();) {
            auto option_iter = data.option.find(it->name);
            if (option_iter == data.option.end()) {
                LogWarn << "Option not found in interface, removing from config" << VAR(it->name);
                it = opts.erase(it);
                erased = true;
                continue;
            }

            const auto& data_option = option_iter->second;
            bool valid = true;

            switch (data_option.type) {
            case InterfaceData::Option::Type::Select:
            case InterfaceData::Option::Type::Switch: {
                if (!it->value.empty()) {
                    auto case_iter = std::ranges::find(data_option.cases, it->value, std::mem_fn(&InterfaceData::Option::Case::name));
                    if (case_iter == data_option.cases.end()) {
                        LogWarn << "Option case not found, removing from config" << VAR(it->name) << VAR(it->value);
                        valid = false;
                    }
                }
            } break;
            case InterfaceData::Option::Type::Checkbox: {
                auto deduped_values = unique_values(it->values);
                if (deduped_values.size() != it->values.size()) {
                    LogWarn << "Duplicate checkbox selections, removing duplicates" << VAR(it->name) << VAR(it->values.size())
                            << VAR(deduped_values.size());
                    it->values = std::move(deduped_values);
                    erased = true;
                }

                const bool count_valid = checkbox_selection_is_valid(data_option, it->values);
                if (!count_valid) {
                    LogWarn << "Checkbox selection count is invalid, removing from config" << VAR(it->name)
                            << VAR(valid_unique_checkbox_count(data_option, it->values));
                }

                for (const auto& val : it->values) {
                    auto case_iter = std::ranges::find(data_option.cases, val, std::mem_fn(&InterfaceData::Option::Case::name));
                    if (case_iter == data_option.cases.end()) {
                        LogWarn << "Checkbox case not found, removing from config" << VAR(it->name) << VAR(val);
                        valid = false;
                        break;
                    }
                }
                valid = valid && count_valid;
            } break;
            case InterfaceData::Option::Type::Input:
                for (auto input_it = it->inputs.begin(); input_it != it->inputs.end();) {
                    const auto& input_name = input_it->first;
                    const bool found = std::ranges::any_of(data_option.inputs, [&](const auto& input) { return input.name == input_name; });
                    if (found) {
                        ++input_it;
                    }
                    else {
                        LogWarn << "Input not found in interface, removing from config" << VAR(it->name) << VAR(input_name);
                        input_it = it->inputs.erase(input_it);
                        erased = true;
                    }
                }
                break;
            }

            if (valid) {
                ++it;
            }
            else {
                it = opts.erase(it);
                erased = true;
            }
        }
    };
    check_option_list(config.global_option);
    check_option_list(config.resource_option);
    check_option_list(config.controller_option);

    auto pretask_identifier = [](const InterfaceData::Pretask& pretask) {
        return pretask.name.empty() ? pretask.exec : pretask.name;
    };

    for (auto pretask_iter = config.pretask.begin(); pretask_iter != config.pretask.end();) {
        check_option_list(pretask_iter->option);

        const auto data_pretasks = Parser::flatten_pretask(data.pretask);
        auto data_pretask_iter =
            std::ranges::find_if(data_pretasks, [&](const auto& pretask) { return pretask_identifier(pretask) == pretask_iter->name; });
        if (data_pretask_iter == data_pretasks.end()) {
            LogWarn << "Pretask not found in interface, removing from config" << VAR(pretask_iter->name);
            pretask_iter = config.pretask.erase(pretask_iter);
            erased = true;
            continue;
        }

        ++pretask_iter;
    }

    return !erased;
}

bool Parser::check_task(const InterfaceData& data, const Configuration& config, Configuration::Task& config_task, bool& changed)
{
    auto data_iter = std::ranges::find(data.task, config_task.name, std::mem_fn(&InterfaceData::Task::name));
    if (data_iter == data.task.end()) {
        LogWarn << "Task not found" << VAR(config_task.name);
        return false;
    }
    if (!task_is_applicable(*data_iter, config)) {
        return true;
    }

    auto is_top_level_option = [&](const std::string& name) {
        return std::ranges::find(data_iter->option, name) != data_iter->option.end();
    };
    auto erase_invalid_option_subtree = [&](std::vector<Configuration::Option>::iterator invalid_iter) {
        auto subtree_end = invalid_iter;
        ++subtree_end;
        while (subtree_end != config_task.option.end() && !is_top_level_option(subtree_end->name)) {
            ++subtree_end;
        }
        return config_task.option.erase(invalid_iter, subtree_end);
    };

    for (auto iter = config_task.option.begin(); iter != config_task.option.end();) {
        if (is_top_level_option(iter->name)) {
            ++iter;
            continue;
        }

        auto option_iter = data.option.find(iter->name);
        if (option_iter != data.option.end() && !option_is_applicable(option_iter->second, config)) {
            LogWarn << "Inapplicable task option found, removing it" << VAR(config_task.name) << VAR(iter->name);
            iter = config_task.option.erase(iter);
            changed = true;
            continue;
        }

        ++iter;
    }

    for (auto config_option_iter = config_task.option.begin(); config_option_iter != config_task.option.end();) {
        auto& config_option = *config_option_iter;
        auto option_iter = data.option.find(config_option.name);
        if (!is_top_level_option(config_option.name)) {
            LogWarn << "Option is not a task-level option, removing it" << VAR(config_task.name) << VAR(config_option.name);
            config_option_iter = erase_invalid_option_subtree(config_option_iter);
            changed = true;
            continue;
        }

        if (option_iter == data.option.end()) {
            LogWarn << "Option not found in interface, removing from task" << VAR(config_task.name) << VAR(config_option.name);
            config_option_iter = erase_invalid_option_subtree(config_option_iter);
            changed = true;
            continue;
        }

        auto subtree_end = config_option_iter;
        if (match_task_option_subtree(data, config, config_option.name, subtree_end, config_task.option.end(), changed)) {
            if (subtree_end != config_task.option.end() && !is_top_level_option(subtree_end->name)) {
                LogWarn << "Extra stale child options found, removing the task option subtree" << VAR(config_task.name)
                        << VAR(config_option.name);
                config_option_iter = erase_invalid_option_subtree(config_option_iter);
                changed = true;
                continue;
            }

            config_option_iter = subtree_end;
            continue;
        }

        LogWarn << "Invalid task option subtree, removing it" << VAR(config_task.name) << VAR(config_option.name);
        config_option_iter = erase_invalid_option_subtree(config_option_iter);
        changed = true;
    }

    return true;
}

MAA_PROJECT_INTERFACE_NS_END
