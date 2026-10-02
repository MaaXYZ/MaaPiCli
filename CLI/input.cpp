#include "input.h"

#include <algorithm>
#include <format>
#include <iostream>
#include <limits>
#include <ranges>
#include <sstream>
#include <string>

#include "MaaUtils/Platform.h"

#ifdef _WIN32
std::string code_page_to_utf8(std::string_view bytes, unsigned int code_page)
{
    if (code_page == CP_UTF8) {
        return std::string(bytes);
    }

    if (bytes.empty()) {
        return std::string();
    }

    const int bytes_size = static_cast<int>(bytes.size());

    const DWORD flags = (code_page == 54936) ? MB_ERR_INVALID_CHARS : 0;

    const int wlen = MultiByteToWideChar(code_page, flags, bytes.data(), bytes_size, nullptr, 0);
    if (wlen <= 0) {
        return std::string(bytes);
    }

    std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
    const int converted = MultiByteToWideChar(code_page, flags, bytes.data(), bytes_size, wbuf.data(), wlen);
    if (converted <= 0) {
        return std::string(bytes);
    }

    if (flags == 0) {
        const bool best_fit = std::ranges::any_of(wbuf, [](wchar_t c) { return c == 0xFFFD || (c >= 0xE000 && c <= 0xF8FF); });
        if (best_fit) {
            return std::string(bytes);
        }
    }

    const int wbuf_size = static_cast<int>(wbuf.size());

    const int u8len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wbuf.data(), wbuf_size, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) {
        return std::string(bytes);
    }

    std::string result(static_cast<size_t>(u8len), '\0');
    const int converted8 =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wbuf.data(), wbuf_size, result.data(), u8len, nullptr, nullptr);
    if (converted8 <= 0) {
        return std::string(bytes);
    }

    return result;
}
#endif

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

bool is_stdin_stream(std::istream& input_stream)
{
    return input_stream.rdbuf() == std::cin.rdbuf();
}

#ifdef _WIN32
bool is_stdin_console()
{
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return false;
    }

    DWORD mode = 0;
    return GetConsoleMode(handle, &mode) != 0;
}

std::string console_input_to_utf8(std::string_view bytes)
{
    return code_page_to_utf8(bytes, GetConsoleCP());
}
#endif

std::string normalize_input_line(std::string line, std::istream& input_stream)
{
#ifdef _WIN32
    if (is_stdin_stream(input_stream) && is_stdin_console()) {
        return console_input_to_utf8(line);
    }
#else
    (void)input_stream;
#endif
    return line;
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
    return normalize_input_line(std::move(line), input_stream);
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
