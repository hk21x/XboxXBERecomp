#include "core/settings.h"

#include <fstream>
#include <sstream>

#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace launcher {

namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// a json value as TOML text: a string quoted, a number or a bool as it is
bool TomlText(const json& v, std::string& out) {
    if (v.is_boolean()) out = v.get<bool>() ? "true" : "false";
    else if (v.is_number_integer()) out = std::to_string(v.get<int64_t>());
    else if (v.is_string()) out = json(v.get<std::string>()).dump();  // a json string is a TOML basic string
    else return false;
    return true;
}

// a top-level line's key and value ("key = value"); false for a table header, a comment or anything else
bool KeyLine(const std::string& line, std::string& key, std::string& value) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == '#' || t[0] == '[') return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = Trim(t.substr(0, eq));
    value = Trim(t.substr(eq + 1));
    if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'') && key.back() == key.front())
        key = key.substr(1, key.size() - 2);
    return !key.empty();
}

bool TableLine(const std::string& line) {
    std::string t = Trim(line);
    return !t.empty() && t[0] == '[';
}

std::string Unquote(const std::string& v) {
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') return v.substr(1, v.size() - 2);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        json j = json::parse(v, nullptr, false);
        if (j.is_string()) return j.get<std::string>();
    }
    return v;
}

}  // namespace

bool ParseLauncherSettings(const std::string& json_text, std::vector<LauncherSetting>& out, std::string& error) {
    const json a = json::parse(json_text, nullptr, false);
    if (!a.is_array()) {
        error = "game.json's settings must be a list";
        return false;
    }
    for (const json& e : a) {
        LauncherSetting s;
        s.key = e.value("key", "");
        s.label = e.value("label", s.key);
        s.help = e.value("help", "");
        if (s.key.empty() || !e.contains("default") || !TomlText(e["default"], s.def)) {
            error = "game.json: each setting needs a key and a default (true, false, a number or text)";
            return false;
        }
        if (e.contains("choices")) {
            for (const json& c : e["choices"]) {
                std::string v;
                if (!c.is_array() || c.size() != 2 || !TomlText(c[0], v) || !c[1].is_string()) {
                    error = "game.json: setting " + s.key + "'s choices are [value, name] pairs";
                    return false;
                }
                s.values.push_back(v);
                s.names.push_back(c[1].get<std::string>());
            }
        } else if (!e["default"].is_boolean()) {
            error = "game.json: setting " + s.key + " needs choices, or a true or false default";
            return false;
        }
        out.push_back(std::move(s));
    }
    return true;
}

bool SameValue(const std::string& a, const std::string& b) { return Unquote(Trim(a)) == Unquote(Trim(b)); }

std::map<std::string, std::string> ReadSettingsFile(const fs::path& file) {
    std::map<std::string, std::string> out;
    std::ifstream in(file);
    std::string line, key, value;
    while (std::getline(in, line)) {
        if (TableLine(line)) break;
        if (KeyLine(line, key, value)) out[key] = value;
    }
    return out;
}

bool WriteSettingsFile(const fs::path& file, const std::map<std::string, std::string>& values, std::string& error) {
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    std::map<std::string, std::string> left = values;
    size_t first_table = lines.size();
    for (size_t i = 0; i < lines.size(); i++) {
        if (TableLine(lines[i])) {
            first_table = i;
            break;
        }
        std::string key, value;
        if (!KeyLine(lines[i], key, value)) continue;
        auto it = left.find(key);
        if (it == left.end()) continue;
        size_t indent = lines[i].find_first_not_of(" \t");
        lines[i] = lines[i].substr(0, indent) + key + " = " + it->second;
        left.erase(it);
    }
    std::vector<std::string> added;
    for (auto& [key, value] : left) added.push_back(key + " = " + value);
    // before the first table, with the blank line toml++ leaves above it kept above the table
    size_t at = first_table;
    if (at < lines.size() && at > 0 && Trim(lines[at - 1]).empty()) at--;
    lines.insert(lines.begin() + std::ptrdiff_t(at), added.begin(), added.end());

    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".launcher";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        for (const std::string& l : lines) o << l << "\n";
        if (!o.flush()) {
            error = "The settings could not be written to " + tmp.string() + ".";
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        error = "The settings could not be saved (" + file.string() + " is in use?). Close the game and try again.";
        return false;
    }
    return true;
}

}  // namespace launcher
