#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace MaaPiCliTerminal
{
// Remove terminal commands as complete sequences, rather than leaving their
// printable payload behind. Preserve valid UTF-8 text. Single-line fields cannot
// inject extra lines; message bodies may retain newlines and tabs.
inline std::string sanitize(std::string_view text, bool multiline = false)
{
    // Decode complete UTF-8 characters so continuation bytes in ordinary text
    // are never mistaken for C1 controls. Express C1 as equivalent ESC forms
    // so the sequence parser below also consumes CSI/OSC payloads and C1 ST.
    std::string normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            normalized.push_back(text[i++]);
            continue;
        }
        const size_t length = lead >= 0xc2 && lead <= 0xdf ? 2 : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (length == 0 || length > text.size() - i) {
            ++i; // Discard invalid UTF-8, including standalone C1 bytes.
            continue;
        }
        uint32_t codepoint = lead & (0x7f >> length);
        bool valid = true;
        for (size_t j = 1; j < length; ++j) {
            const auto continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if (!valid || (length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000)
            || (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
            ++i;
            continue;
        }
        if (codepoint >= 0x80 && codepoint <= 0x9f) {
            normalized.push_back(0x1b);
            normalized.push_back(static_cast<char>(codepoint - 0x40));
        }
        else {
            normalized.append(text.substr(i, length));
        }
        i += length;
    }
    text = normalized;

    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == 0x1b) {
            if (++i == text.size()) {
                break;
            }
            const char command = text[i];
            if (command == '[') { // CSI, terminated by a final byte.
                while (++i < text.size()) {
                    if (text[i] >= 0x40 && text[i] <= 0x7e) {
                        break;
                    }
                }
            }
            else if (command == ']' || command == 'P' || command == 'X' || command == '^' || command == '_') {
                // OSC/DCS/SOS/PM/APC, terminated by ST (or BEL for OSC).
                while (++i < text.size()) {
                    if ((command == ']' && text[i] == '\a') || (text[i] == 0x1b && i + 1 < text.size() && text[i + 1] == '\\')) {
                        if (text[i] == 0x1b) {
                            ++i;
                        }
                        break;
                    }
                }
            }
            else { // ESC sequences with optional intermediate bytes.
                while (i < text.size() && text[i] >= 0x20 && text[i] <= 0x2f) {
                    ++i;
                }
            }
            continue;
        }
        if (c == 0x7f || (c < 0x20 && !(multiline && (c == '\n' || c == '\t')))) {
            continue;
        }
        result.push_back(text[i]);
    }
    return result;
}
}
