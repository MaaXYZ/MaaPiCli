#include "ProjectInterface/Parser.h"

#include <functional>
#include <ranges>
#include <type_traits>
#include <unordered_set>

#include "MaaUtils/Logger.h"

MAA_PROJECT_INTERFACE_NS_BEGIN

namespace
{
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
        return checkbox_selection_is_valid(option, *defaults);
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

    // check controller type
    for (const auto& ctrl : data.controller) {
        if (ctrl.type == InterfaceData::Controller::Type::Invalid) {
            LogError << "Invalid Controller Type" << VAR(ctrl.type);
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
    auto data_opt = deserialize_interface(json);
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
    auto data = deserialize_interface(json);
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

    std::string error_key;
    if (!Configuration().check_json(json, error_key)) {
        LogError << "json is not a Configuration" << VAR(error_key) << VAR(json);
        return std::nullopt;
    }

    return json.as<Configuration>();
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
        bool checked = check_task(data, *iter, task_changed);
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

bool Parser::check_task(const InterfaceData& data, Configuration::Task& config_task, bool& changed)
{
    auto data_iter = std::ranges::find(data.task, config_task.name, std::mem_fn(&InterfaceData::Task::name));
    if (data_iter == data.task.end()) {
        LogWarn << "Task not found" << VAR(config_task.name);
        return false;
    }

    for (auto& config_option : config_task.option) {
        auto option_iter = data.option.find(config_option.name);
        if (option_iter == data.option.end()) {
            LogWarn << "Option not found" << VAR(config_task.name) << VAR(config_option.name);
            return false;
        }

        const InterfaceData::Option& data_option = option_iter->second;

        switch (data_option.type) {
        case InterfaceData::Option::Type::Select:
        case InterfaceData::Option::Type::Switch: {
            auto case_iter = std::ranges::find(data_option.cases, config_option.value, std::mem_fn(&InterfaceData::Option::Case::name));
            if (case_iter == data_option.cases.end()) {
                LogWarn << "Case not found" << VAR(config_task.name) << VAR(config_option.name) << VAR(config_option.value);
                return false;
            }
        } break;
        case InterfaceData::Option::Type::Checkbox: {
            auto deduped_values = unique_values(config_option.values);
            if (deduped_values.size() != config_option.values.size()) {
                LogWarn << "Duplicate checkbox selections, removing duplicates" << VAR(config_task.name) << VAR(config_option.name)
                        << VAR(config_option.values.size()) << VAR(deduped_values.size());
                config_option.values = std::move(deduped_values);
                changed = true;
            }

            if (!checkbox_selection_is_valid(data_option, config_option.values)) {
                LogWarn << "Checkbox selection count is invalid" << VAR(config_task.name) << VAR(config_option.name);
                return false;
            }

            for (const auto& val : config_option.values) {
                auto case_iter = std::ranges::find(data_option.cases, val, std::mem_fn(&InterfaceData::Option::Case::name));
                if (case_iter == data_option.cases.end()) {
                    LogWarn << "Checkbox case not found" << VAR(config_task.name) << VAR(config_option.name) << VAR(val);
                    return false;
                }
            }
        } break;
        case InterfaceData::Option::Type::Input:
            break;
        }
    }

    return true;
}

MAA_PROJECT_INTERFACE_NS_END
