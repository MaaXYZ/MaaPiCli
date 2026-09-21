#pragma once

#include <iostream>
#include <istream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// These helpers distinguish an empty line, which can select a default, from a
// closed input stream. Nullopt means the interaction was aborted.
[[nodiscard]] std::optional<std::string>
    read_line(std::string_view prompt = { }, std::istream& input_stream = std::cin, std::ostream& output_stream = std::cout);

[[nodiscard]] std::optional<int> input(
    size_t size,
    std::string_view prompt = "Please input",
    int default_value = 0,
    std::istream& input_stream = std::cin,
    std::ostream& output_stream = std::cout);

// When allow_empty_selection is true, a standalone 0 is an explicit empty selection.
[[nodiscard]] std::optional<std::vector<int>> input_multi(
    size_t size,
    std::string_view prompt = "Please input multiple",
    std::span<const int> defaults = { },
    std::istream& input_stream = std::cin,
    std::ostream& output_stream = std::cout,
    bool allow_empty_selection = false);
