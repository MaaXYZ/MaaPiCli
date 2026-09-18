#include "CLI/input.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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
}

int main()
{
    {
        std::istringstream input_stream("\nnext");
        std::ostringstream output_stream;
        const auto line = read_line("Name: ", input_stream, output_stream);
        require(line.has_value() && line->empty(), "an empty line should be valid input");
        require(output_stream.str() == "Name: ", "read_line should write its prompt");
    }
    {
        std::istringstream input_stream;
        std::ostringstream output_stream;
        require(!read_line({ }, input_stream, output_stream).has_value(), "a closed stream should abort read_line");
    }
    {
        std::istringstream input_stream("bad\n\n");
        std::ostringstream output_stream;
        const auto selected = input(3, "Choose", 2, input_stream, output_stream);
        require(selected.has_value() && *selected == 2, "invalid input should retry and empty input should use the default");
    }
    {
        std::istringstream input_stream("1 2\n2\n");
        std::ostringstream output_stream;
        const auto selected = input(3, "Choose", 0, input_stream, output_stream);
        require(selected.has_value() && *selected == 2, "single selection should reject multiple values and retry");
    }
    {
        std::istringstream input_stream("bad\n");
        std::ostringstream output_stream;
        require(!input(3, "Choose", 0, input_stream, output_stream).has_value(), "a closed stream should abort input");
    }
    {
        std::istringstream input_stream("1 3\n");
        std::ostringstream output_stream;
        const int defaults[] = { 2 };
        const auto selected = input_multi(3, "Choose", defaults, input_stream, output_stream);
        require(selected.has_value() && *selected == std::vector<int>({ 1, 3 }), "multi-selection should accept several values");
    }
    {
        std::istringstream input_stream("\n");
        std::ostringstream output_stream;
        const auto selected = input_multi(3, "Choose", { }, input_stream, output_stream, true);
        require(selected.has_value() && selected->empty(), "an empty line should produce an empty multi-selection");
    }
    {
        std::istringstream input_stream("0\n");
        std::ostringstream output_stream;
        const int defaults[] = { 2 };
        const auto selected = input_multi(3, "Choose", defaults, input_stream, output_stream, true);
        require(selected.has_value() && selected->empty(), "zero should clear a defaulted multi-selection");
    }
    {
        std::istringstream input_stream("\n");
        std::ostringstream output_stream;
        const int defaults[] = { 2 };
        const auto selected = input_multi(3, "Choose", defaults, input_stream, output_stream, true);
        require(selected.has_value() && *selected == std::vector<int>({ 2 }), "empty input should keep a multi-selection default");
    }
    {
        std::istringstream input_stream("0\n2\n");
        std::ostringstream output_stream;
        const auto selected = input(3, "Choose", 2, input_stream, output_stream);
        require(selected.has_value() && *selected == 2, "zero should not clear a selection when empty input is invalid");
    }
    {
        std::istringstream input_stream("\n1\n");
        std::ostringstream output_stream;
        const auto selected = input_multi(3, "Choose", { }, input_stream, output_stream);
        require(
            selected.has_value() && *selected == std::vector<int>({ 1 }),
            "an empty multi-selection without an allowed empty selection should retry");
    }

    if (failures != 0) {
        std::cerr << failures << " input test assertion(s) failed\n";
        return 1;
    }

    std::cout << "MaaPiCli input tests passed\n";
    return 0;
}
