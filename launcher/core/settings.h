#pragma once
// The game's settings the launcher offers before it starts (game.json "settings"), read from and written to the
// settings file the game keeps beside its exe (play\settings.toml, the runtime's app/settings.cpp). Only the file's
// top-level keys are touched, and only the ones the player changes: the rest of the file (the key tables, the
// settings the launcher does not show) stays as the game wrote it. A file not there yet gets those keys alone; the
// game fills in the rest the first time it saves.
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace launcher {

struct LauncherSetting {
    std::string key;    // the settings file's key ("skip_intro")
    std::string label;  // "Skip intro movies"
    std::string help;   // a line under it, optional
    // each choice as TOML text (true, 60, "borderless") and its name in the list; a setting with no values listed
    // and a true or false default is a checkbox
    std::vector<std::string> values, names;
    std::string def;    // the game's default, as TOML text: shown when the file has no such key
    bool Checkbox() const { return values.empty(); }
};

// game.json "settings" (its text): [{ "key", "label", "help", "default", "choices": [[value, name], ...] }]
bool ParseLauncherSettings(const std::string& json_text, std::vector<LauncherSetting>& out, std::string& error);

// the file's top-level keys, each value as its TOML text; nothing when there is no file
std::map<std::string, std::string> ReadSettingsFile(const std::filesystem::path& file);
// each key set to its TOML text: replaced where the top level has it, else added before the first table
bool WriteSettingsFile(const std::filesystem::path& file, const std::map<std::string, std::string>& values,
                       std::string& error);
// two TOML values the same ("x" and 'x', 60 and 60)
bool SameValue(const std::string& a, const std::string& b);

}  // namespace launcher
