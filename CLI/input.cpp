#include "input.h"

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <sstream>

#include "MaaUtils/Platform.h"

namespace
{
std::optional<std::vector<int>> parse_multi_selection(const std::string& buffer, size_t size, bool allow_empty_selection)
{
    if (allow_empty_selection && buffer == "0") {
        return std::vector<int> { };
    }

    if (!std::ranges::all_of(buffer, [](unsigned char c) { return std::isdigit(c) || std::isspace(c); })) {
        return std::nullopt;
    }

    std::istringstream stream(buffer);
    std::vector<int> values;
    while (true) {
        stream >> std::ws;
        if (stream.eof()) {
            break;
        }

        size_t value = 0;
        if (!(stream >> value) || value == 0 || value > size || value > static_cast<size_t>(std::numeric_limits<int>::max())) {
            return std::nullopt;
        }
        values.emplace_back(static_cast<int>(value));
    }

    if (values.empty()) {
        return std::nullopt;
    }
    return values;
}

bool has_valid_default(std::span<const int> defaults, size_t size)
{
    return !defaults.empty() && std::ranges::all_of(defaults, [&](int value) { return value >= 1 && static_cast<size_t>(value) <= size; });
}

std::optional<std::vector<int>> input_multi_impl(
    size_t size,
    std::string_view prompt,
    std::span<const int> defaults,
    bool allow_multiple,
    bool allow_empty_selection,
    std::istream& input_stream,
    std::ostream& output_stream)
{
    output_stream << std::format("{} [1-{}]: ", prompt, size);

    while (true) {
        auto line = read_line({ }, input_stream, output_stream);
        if (!line) {
            return std::nullopt;
        }

        if (line->empty() && has_valid_default(defaults, size)) {
            output_stream << '\n';
            return std::vector<int>(defaults.begin(), defaults.end());
        }

        if (line->empty() && allow_empty_selection) {
            output_stream << '\n';
            return std::vector<int> { };
        }

        auto values = parse_multi_selection(*line, size, allow_empty_selection);
        if (values && (allow_multiple || values->size() == 1)) {
            output_stream << '\n';
            return values;
        }
        output_stream << std::format("Invalid value, {} [1-{}]: ", prompt, size);
    }
}
}

std::optional<std::string> read_line(std::string_view prompt, std::istream& input_stream, std::ostream& output_stream)
{
    if (!prompt.empty()) {
        output_stream << prompt;
    }

    input_stream.sync();
    std::string line;
    std::getline(input_stream, line);
    if (!input_stream) {
        return std::nullopt;
    }
    //适配input选项的中文正则校验
    return MAA_NS::crt_to_utf8(line);
}

std::optional<std::vector<int>> input_multi(
    size_t size,
    std::string_view prompt,
    std::span<const int> defaults,
    std::istream& input_stream,
    std::ostream& output_stream,
    bool allow_empty_selection)
{
    return input_multi_impl(size, prompt, defaults, true, allow_empty_selection, input_stream, output_stream);
}

std::optional<int> input(size_t size, std::string_view prompt, int default_value, std::istream& input_stream, std::ostream& output_stream)
{
    const int default_values[] = { default_value };
    const auto defaults =
        default_value >= 1 && static_cast<size_t>(default_value) <= size ? std::span<const int>(default_values) : std::span<const int> { };

    auto values = input_multi_impl(size, prompt, defaults, false, false, input_stream, output_stream);
    return values ? std::optional<int>(values->front()) : std::nullopt;
}
