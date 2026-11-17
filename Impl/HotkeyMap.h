#pragma once

// v2.8.0: map human-readable hotkey combos (e.g. "Ctrl+Shift+A") to per-controller
// virtual key codes, following the key code tables used by ClickKey / KeyDown actions.

#include <optional>
#include <string>
#include <vector>

#include "ProjectInterface/Types.h"

namespace MaaPiCliHotkey
{
enum class KeycodeSet
{
    Win32Vk,    // Win32 Virtual-Key codes (Win32, Gamepad, Linux with use_win32_vk_code)
    LinuxEvdev, // Linux evdev scan codes (Linux default input methods)
    Android,    // Android keycodes (Adb, IOS)
    MacVirtual, // macOS virtual key codes (MacOS, PlayCover)
};

struct Combo
{
    std::string canonical;      // normalized human-readable form, e.g. "Ctrl+Shift+A"
    int primary = -1;           // virtual key code of the primary key
    std::vector<int> modifiers; // 0..2 modifier codes, in combo order
};

// Resolve the table from the selected interface controller, including Linux's VK override.
KeycodeSet keycode_set(const MAA_PROJECT_INTERFACE_NS::InterfaceData& data, const MAA_PROJECT_INTERFACE_NS::Configuration& config);

// Validate every declared field (including defaults) and store canonical strings.
bool normalize_hotkeys(
    const MAA_PROJECT_INTERFACE_NS::InterfaceData::Option& definition,
    MAA_PROJECT_INTERFACE_NS::Configuration::Option& option,
    KeycodeSet set);

// Split and resolve a combo string. Returns nullopt when a token is unknown
// for the given keycode set. combo like "Ctrl+Shift+A"; last token is primary.
std::optional<Combo> parse(const std::string& combo, KeycodeSet set);

// Resolve a single key token (case-insensitive, aliases allowed) to a key code.
std::optional<int> find_keycode(const std::string& token, KeycodeSet set);
}
