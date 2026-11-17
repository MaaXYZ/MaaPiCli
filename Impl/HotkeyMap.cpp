#include "HotkeyMap.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace MaaPiCliHotkey
{
KeycodeSet keycode_set(const MAA_PROJECT_INTERFACE_NS::InterfaceData& data, const MAA_PROJECT_INTERFACE_NS::Configuration& config)
{
    using Type = MAA_PROJECT_INTERFACE_NS::InterfaceData::Controller::Type;
    for (const auto& controller : data.controller) {
        if (controller.name != config.controller.name) {
            continue;
        }
        switch (controller.type) {
        case Type::Linux:
            return controller.lnx.use_win32_vk_code ? KeycodeSet::Win32Vk : KeycodeSet::LinuxEvdev;
        case Type::Adb:
        case Type::IOS:
            return KeycodeSet::Android;
        case Type::MacOS:
        case Type::PlayCover:
            return KeycodeSet::MacVirtual;
        default:
            return KeycodeSet::Win32Vk;
        }
    }
    return KeycodeSet::Win32Vk;
}

bool normalize_hotkeys(
    const MAA_PROJECT_INTERFACE_NS::InterfaceData::Option& definition,
    MAA_PROJECT_INTERFACE_NS::Configuration::Option& option,
    KeycodeSet set)
{
    auto normalized = option.hotkeys;
    for (const auto& field : definition.hotkeys) {
        const auto it = normalized.find(field.name);
        const auto combo = parse(it == normalized.end() ? field.default_ : it->second, set);
        if (!combo) {
            return false;
        }
        normalized[field.name] = combo->canonical;
    }
    option.hotkeys = std::move(normalized);
    return true;
}

namespace
{
// -1 means the key is not available on that platform's code table.
struct KeyEntry
{
    std::string_view canonical;              // display name used in the normalized combo
    std::array<std::string_view, 3> aliases; // alternative accepted spellings, "" padded
    int win32 = -1;
    int evdev = -1;
    int android = -1;
    int mac = -1;
};

constexpr int kInvalid = -1;

constexpr std::array<KeyEntry, 78> kKeys { {
    // letters
    { "A", { }, 0x41, 30, 29, 0x00 },
    { "B", { }, 0x42, 48, 30, 0x0B },
    { "C", { }, 0x43, 46, 31, 0x08 },
    { "D", { }, 0x44, 32, 32, 0x02 },
    { "E", { }, 0x45, 18, 33, 0x0E },
    { "F", { }, 0x46, 33, 34, 0x03 },
    { "G", { }, 0x47, 34, 35, 0x05 },
    { "H", { }, 0x48, 35, 36, 0x04 },
    { "I", { }, 0x49, 23, 37, 0x22 },
    { "J", { }, 0x4A, 36, 38, 0x26 },
    { "K", { }, 0x4B, 37, 39, 0x28 },
    { "L", { }, 0x4C, 38, 40, 0x25 },
    { "M", { }, 0x4D, 50, 41, 0x2E },
    { "N", { }, 0x4E, 49, 42, 0x2D },
    { "O", { }, 0x4F, 24, 43, 0x1F },
    { "P", { }, 0x50, 25, 44, 0x23 },
    { "Q", { }, 0x51, 16, 45, 0x0C },
    { "R", { }, 0x52, 19, 46, 0x0F },
    { "S", { }, 0x53, 31, 47, 0x01 },
    { "T", { }, 0x54, 20, 48, 0x11 },
    { "U", { }, 0x55, 22, 49, 0x20 },
    { "V", { }, 0x56, 47, 50, 0x09 },
    { "W", { }, 0x57, 17, 51, 0x0D },
    { "X", { }, 0x58, 45, 52, 0x07 },
    { "Y", { }, 0x59, 21, 53, 0x10 },
    { "Z", { }, 0x5A, 44, 54, 0x06 },
    // digits
    { "0", { }, 0x30, 11, 7, 0x1D },
    { "1", { }, 0x31, 2, 8, 0x12 },
    { "2", { }, 0x32, 3, 9, 0x13 },
    { "3", { }, 0x33, 4, 10, 0x14 },
    { "4", { }, 0x34, 5, 11, 0x15 },
    { "5", { }, 0x35, 6, 12, 0x17 },
    { "6", { }, 0x36, 7, 13, 0x16 },
    { "7", { }, 0x37, 8, 14, 0x1A },
    { "8", { }, 0x38, 9, 15, 0x1C },
    { "9", { }, 0x39, 10, 16, 0x19 },
    // function keys
    { "F1", { }, 0x70, 59, 131, 0x7A },
    { "F2", { }, 0x71, 60, 132, 0x78 },
    { "F3", { }, 0x72, 61, 133, 0x63 },
    { "F4", { }, 0x73, 62, 134, 0x76 },
    { "F5", { }, 0x74, 63, 135, 0x60 },
    { "F6", { }, 0x75, 64, 136, 0x61 },
    { "F7", { }, 0x76, 65, 137, 0x62 },
    { "F8", { }, 0x77, 66, 138, 0x64 },
    { "F9", { }, 0x78, 67, 139, 0x65 },
    { "F10", { }, 0x79, 68, 140, 0x6D },
    { "F11", { }, 0x7A, 87, 141, 0x67 },
    { "F12", { }, 0x7B, 88, 142, 0x6F },
    // modifiers
    { "Ctrl", { "Control" }, 0x11, 29, 113, 0x3B },
    { "Alt", { "Option" }, 0x12, 56, 57, 0x3A },
    { "Shift", { }, 0x10, 42, 59, 0x38 },
    { "Meta", { "Win", "Cmd", "Super" }, 0x5B, 125, 117, 0x37 },
    // editing / navigation
    { "Enter", { "Return" }, 0x0D, 28, 66, 0x24 },
    { "Esc", { "Escape" }, 0x1B, 1, 111, 0x35 },
    { "Space", { "Spacebar" }, 0x20, 57, 62, 0x31 },
    { "Tab", { }, 0x09, 15, 61, 0x30 },
    { "Backspace", { }, 0x08, 14, 67, 0x33 },
    { "Insert", { }, 0x2D, 110, 124, kInvalid },
    { "Delete", { }, 0x2E, 111, 112, 0x75 },
    { "Home", { }, 0x24, 102, 122, 0x73 },
    { "End", { }, 0x23, 107, 123, 0x77 },
    { "PageUp", { }, 0x21, 104, 92, 0x74 },
    { "PageDown", { }, 0x22, 109, 93, 0x79 },
    { "Up", { "ArrowUp" }, 0x26, 103, 19, 0x7E },
    { "Down", { "ArrowDown" }, 0x28, 108, 20, 0x7D },
    { "Left", { "ArrowLeft" }, 0x25, 105, 21, 0x7B },
    { "Right", { "ArrowRight" }, 0x27, 106, 22, 0x7C },
    // punctuation
    { ";", { "Semicolon" }, 0xBA, 39, 74, 0x29 },
    { "=", { "Equals" }, 0xBB, 13, 70, 0x18 },
    { ",", { "Comma" }, 0xBC, 51, 55, 0x2B },
    { "-", { "Minus" }, 0xBD, 12, 69, 0x1B },
    { ".", { "Period", "Dot" }, 0xBE, 52, 56, 0x2F },
    { "/", { "Slash" }, 0xBF, 53, 76, 0x2C },
    { "`", { "Grave", "Backquote" }, 0xC0, 41, 68, 0x0A },
    { "[", { "BracketLeft" }, 0xDB, 26, 71, 0x21 },
    { "]", { "BracketRight" }, 0xDD, 27, 72, 0x1E },
    { "\\", { "Backslash" }, 0xDC, 43, 73, 0x2A },
    { "'", { "Quote", "Apostrophe" }, 0xDE, 40, 75, 0x27 },
} };

std::string to_lower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(const std::string& s)
{
    auto begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    auto end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

const KeyEntry* find_entry(const std::string& token, KeycodeSet set)
{
    auto lower = to_lower(token);
    for (const auto& entry : kKeys) {
        auto canonical = to_lower(std::string(entry.canonical));
        bool matched = canonical == lower;
        if (!matched) {
            for (const auto& alias : entry.aliases) {
                if (to_lower(std::string(alias)) == lower) {
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            continue;
        }
        int code = set == KeycodeSet::Win32Vk      ? entry.win32
                   : set == KeycodeSet::LinuxEvdev ? entry.evdev
                   : set == KeycodeSet::Android    ? entry.android
                                                   : entry.mac;
        if (code == kInvalid) {
            return nullptr;
        }
        return &entry;
    }
    return nullptr;
}

int code_of(const KeyEntry& entry, KeycodeSet set)
{
    switch (set) {
    case KeycodeSet::Win32Vk:
        return entry.win32;
    case KeycodeSet::LinuxEvdev:
        return entry.evdev;
    case KeycodeSet::Android:
        return entry.android;
    case KeycodeSet::MacVirtual:
        return entry.mac;
    }
    return kInvalid;
}

bool is_modifier(const KeyEntry& entry)
{
    auto canonical = to_lower(std::string(entry.canonical));
    return canonical == "ctrl" || canonical == "alt" || canonical == "shift" || canonical == "meta";
}
}
} // namespace MaaPiCliHotkey

namespace MaaPiCliHotkey
{
std::optional<int> find_keycode(const std::string& token, KeycodeSet set)
{
    const auto* entry = find_entry(trim(token), set);
    return entry ? std::optional { code_of(*entry, set) } : std::nullopt;
}

std::optional<Combo> parse(const std::string& combo, KeycodeSet set)
{
    std::vector<std::string> tokens;
    std::string current;
    for (char c : combo) {
        if (c == '+') {
            tokens.push_back(trim(current));
            current.clear();
        }
        else {
            current.push_back(c);
        }
    }
    tokens.push_back(trim(current));

    if (tokens.empty() || std::any_of(tokens.begin(), tokens.end(), [](const std::string& t) { return t.empty(); })) {
        return std::nullopt;
    }
    if (tokens.size() > 3) { // primary + at most modifier1/modifier2
        return std::nullopt;
    }

    Combo result;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        const auto* entry = find_entry(tokens[i], set);
        if (!entry || !is_modifier(*entry)) {
            return std::nullopt;
        }
        result.modifiers.push_back(code_of(*entry, set));
        result.canonical += std::string(entry->canonical) + "+";
    }

    const auto* primary = find_entry(tokens.back(), set);
    if (!primary) {
        return std::nullopt;
    }
    result.canonical += std::string(primary->canonical);
    result.primary = code_of(*primary, set);
    return result;
}
}
