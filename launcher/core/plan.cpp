#include "core/plan.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "core/archive.h"
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace launcher {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Replace(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

// '*' any run of characters but '/'
bool Glob(const std::string& pattern, const std::string& name) {
    size_t p = 0, n = 0, star = std::string::npos, back = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++, back = n;
        } else if (p < pattern.size() && pattern[p] == name[n]) {
            p++, n++;
        } else if (star != std::string::npos && name[back] != '/') {
            p = star + 1, n = ++back;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') p++;
    return p == pattern.size();
}

}  // namespace

uint64_t Tool::Bytes() const {
    uint64_t n = 0;
    for (const FileRef& f : files) n += f.size;
    for (const FileRef& f : cabs) n += f.size;
    return n;
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(uint8_t(s[i + 1])) && std::isxdigit(uint8_t(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string Select(const Tool& tool, const std::string& name) {
    std::string n = tool.kind == "vsix" ? UrlDecode(name) : name;
    if (tool.kind == "vsix") {
        if (n.rfind("Contents/", 0) != 0) return {};
        n = n.substr(9);
    }
    n = StripComponents(n, tool.strip);
    if (n.empty() || n.back() == '/') return {};
    if (tool.keep.empty()) return n;
    for (const std::string& k : tool.keep) {
        if (k.back() == '/' ? n.rfind(k, 0) == 0 : Glob(k, n)) return n;
    }
    return {};
}

std::vector<std::string> MsiCabs(const std::vector<uint8_t>& msi) {
    // the Windows SDK's cabinets are named by a 32 digit hash: "0123...cdef.cab"
    std::set<std::string> found;
    for (size_t i = 32; i + 4 <= msi.size(); i++) {
        if (msi[i] != '.' || msi[i + 1] != 'c' || msi[i + 2] != 'a' || msi[i + 3] != 'b') continue;
        bool hex = true;
        for (size_t j = i - 32; j < i && hex; j++) hex = std::isxdigit(msi[j]) && !std::isupper(msi[j]);
        if (hex) found.insert(std::string(msi.begin() + long(i - 32), msi.begin() + long(i + 4)));
    }
    return {found.begin(), found.end()};
}

bool LoadPlan(const std::string& toolchain_json, Plan& plan, std::string& error) {
    json j = json::parse(toolchain_json, nullptr, false);
    if (j.is_discarded() || !j.contains("tools")) {
        error = "toolchain.json is not readable";
        return false;
    }
    for (const json& t : j["tools"]) {
        Tool tool;
        tool.id = t.value("id", "");
        tool.title = t.value("title", tool.id);
        tool.kind = t.value("kind", "zip");
        tool.strip = t.value("strip", 0);
        if (t.contains("keep"))
            for (const json& k : t["keep"]) tool.keep.push_back(k.get<std::string>());
        FileRef f;
        f.url = t.value("url", "");
        f.sha256 = Lower(t.value("sha256", ""));
        f.size = t.value("size", uint64_t(0));
        f.name = f.url.substr(f.url.rfind('/') + 1);
        f.name = UrlDecode(f.name);
        tool.files.push_back(f);
        tool.stamp = f.sha256;
        for (const std::string& k : tool.keep) tool.stamp += " " + k;
        if (tool.id.empty() || f.url.empty() || f.sha256.size() != 64) {
            error = "toolchain.json: tool " + tool.id + " needs an id, a url and a sha256";
            return false;
        }
        plan.tools.push_back(tool);
    }
    return true;
}

bool ResolveMicrosoft(Http& http, const std::string& toolchain_json, Plan& plan, std::string& error) {
    json tc = json::parse(toolchain_json, nullptr, false);
    if (tc.is_discarded() || !tc.contains("msvc") || !tc.contains("sdk")) {
        error = "toolchain.json has no msvc or sdk entry";
        return false;
    }
    const json& msvc = tc["msvc"];
    const json& sdk = tc["sdk"];
    std::string text;
    if (!FetchText(http, msvc.value("channel", "https://aka.ms/vs/17/release/channel"), text, error)) {
        error = "Visual Studio channel: " + error;
        return false;
    }
    json channel = json::parse(text, nullptr, false);
    std::string manifest_url;
    if (!channel.is_discarded())
        for (const json& item : channel.value("channelItems", json::array()))
            if (item.value("id", "") == "Microsoft.VisualStudio.Manifests.VisualStudio" && item.contains("payloads"))
                manifest_url = item["payloads"][0].value("url", "");
    if (manifest_url.empty()) {
        error = "the Visual Studio channel lists no package manifest";
        return false;
    }
    if (!FetchText(http, manifest_url, text, error)) {
        error = "Visual Studio manifest: " + error;
        return false;
    }
    json manifest = json::parse(text, nullptr, false);
    text.clear();
    if (manifest.is_discarded() || !manifest.contains("packages")) {
        error = "the Visual Studio manifest is not readable";
        return false;
    }
    // packages by id (lower case); several share an id (languages, chips): x64 or neutral, English or none
    std::map<std::string, const json*> packages;
    for (const json& p : manifest["packages"]) {
        std::string chip = Lower(p.value("chip", "")), lang = Lower(p.value("language", ""));
        if (!chip.empty() && chip != "x64" && chip != "neutral") continue;
        if (!lang.empty() && lang != "en-us" && lang != "neutral") continue;
        packages.emplace(Lower(p.value("id", "")), &p);
    }
    auto payloads = [](const json& p, std::vector<FileRef>& out) {
        for (const json& pl : p.value("payloads", json::array())) {
            FileRef f;
            f.url = pl.value("url", "");
            f.sha256 = Lower(pl.value("sha256", ""));
            f.size = pl.value("size", uint64_t(0));
            f.name = pl.value("fileName", "");
            size_t slash = f.name.find_last_of("\\/");
            if (slash != std::string::npos) f.name = f.name.substr(slash + 1);
            out.push_back(f);
        }
    };

    // the C++ libraries and headers: vsix packages of one MSVC version
    Tool vc;
    vc.id = "msvc";
    vc.kind = "vsix";
    std::string version = msvc.value("version", "");
    vc.title = "Microsoft C++ libraries " + version;
    for (const json& name : msvc.value("packages", json::array())) {
        std::string id = Replace(name.get<std::string>(), "{v}", version);
        auto it = packages.find(Lower(id));
        if (it == packages.end()) {
            error = "the Visual Studio manifest has no " + id;
            return false;
        }
        std::vector<FileRef> files;
        payloads(*it->second, files);
        for (FileRef& f : files) {
            f.name = id + ".vsix";  // payload names repeat ("payload.vsix")
            vc.files.push_back(f);
        }
    }
    vc.stamp = version;

    // the Windows SDK: some of its .msi files and the .cab files they name
    Tool ws;
    ws.id = "winsdk";
    ws.kind = "msi";
    std::string sdk_id = sdk.value("package", "");
    ws.title = "Windows SDK " + sdk.value("version", "");
    auto it = packages.find(Lower(sdk_id));
    if (it == packages.end()) {
        error = "the Visual Studio manifest has no " + sdk_id;
        return false;
    }
    std::vector<FileRef> all;
    payloads(*it->second, all);
    std::set<std::string> want;
    for (const json& m : sdk.value("msis", json::array())) want.insert(Lower(m.get<std::string>()));
    for (const FileRef& f : all) {
        std::string n = Lower(f.name);
        if (want.count(n)) ws.files.push_back(f);
        else if (n.size() > 4 && n.compare(n.size() - 4, 4, ".cab") == 0) ws.cabs.push_back(f);
    }
    if (ws.files.size() != want.size()) {
        error = "the Windows SDK package lacks some of its installers (" + std::to_string(ws.files.size()) + " of " +
                std::to_string(want.size()) + ")";
        return false;
    }
    // the installers taken, so a longer list extracts it again
    ws.stamp = sdk_id;
    for (const FileRef& f : ws.files) ws.stamp += " " + f.name;
    plan.sdk_version = sdk.value("version", "");
    plan.tools.push_back(vc);
    plan.tools.push_back(ws);
    return true;
}

}  // namespace launcher
