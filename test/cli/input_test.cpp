#include "CLI/input.h"

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
int failures = 0;

std::string to_hex(std::string_view s)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(s.size() * 3);
    for (unsigned char c : s) {
        result.push_back(digits[c >> 4]);
        result.push_back(digits[c & 0x0F]);
        result.push_back(' ');
    }
    return result;
}

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

void require_bytes_equal(const std::string& actual, const std::string& expected, const std::string& message)
{
    if (actual != expected) {
        std::cerr << "FAILED: " << message << '\n'
                  << "  actual  : " << to_hex(actual) << '\n'
                  << "  expected: " << to_hex(expected) << '\n';
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

#ifdef _WIN32
    {
        const std::string utf8_bytes = "\xE6\xB5\x8B\xE8\xAF\x95";
        require_bytes_equal(code_page_to_utf8(utf8_bytes, 65001), utf8_bytes, "CP65001 should pass UTF-8 through");
    }
    {
        const std::string gbk_bytes = "\xB2\xE2\xCA\xD4";
        const std::string expected = "\xE6\xB5\x8B\xE8\xAF\x95";
        require_bytes_equal(code_page_to_utf8(gbk_bytes, 936), expected, "CP936 GBK should convert to UTF-8");
    }
    {
        const std::string sjis_bytes = "\x83\x65\x83\x58\x83\x67";
        const std::string expected = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";
        require_bytes_equal(code_page_to_utf8(sjis_bytes, 932), expected, "CP932 Shift-JIS should convert to UTF-8");
    }
    {
        const std::string cp949_bytes = "\xC5\xD7\xBD\xBA\xC6\xAE";
        const std::string expected = "\xED\x85\x8C\xEC\x8A\xA4\xED\x8A\xB8";
        require_bytes_equal(code_page_to_utf8(cp949_bytes, 949), expected, "CP949 should convert to UTF-8");
    }
    {
        const std::string cp1252_bytes = "\xE9";
        const std::string expected = "\xC3\xA9";
        require_bytes_equal(code_page_to_utf8(cp1252_bytes, 1252), expected, "CP1252 should convert to UTF-8");
    }
    {
        require(code_page_to_utf8("", 936).empty(), "empty input should stay empty under CP936");
        require(code_page_to_utf8("", 65001).empty(), "empty input should stay empty under CP65001");
    }
    {
        const std::string bad = "\xFF";
        require_bytes_equal(code_page_to_utf8(bad, 936), bad, "invalid bytes should be returned as-is");
    }
#endif

    if (failures != 0) {
        std::cerr << failures << " input test assertion(s) failed\n";
        return 1;
    }

    std::cout << "MaaPiCli input tests passed\n";
    return 0;
}
