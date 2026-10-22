#pragma once
// What differs between Windows (the launcher) and Linux (its test tool): running programs, .msi extraction, links
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace launcher {

class Platform {
public:
    virtual ~Platform() = default;
    // a program with its arguments in cwd, env entries replacing the inherited ones; each output line (stdout and
    // stderr) to line; its exit code, -1 when it could not start or was cancelled
    virtual int Run(const std::vector<std::string>& args, const std::filesystem::path& cwd,
                    const std::map<std::string, std::string>& env, const std::function<void(const std::string&)>& line,
                    const std::atomic<bool>* cancel) = 0;
    // an .msi's files under target (an administrative install: no rights needed, nothing registered)
    virtual bool ExtractMsi(const std::filesystem::path& msi, const std::filesystem::path& target, std::string& error) = 0;
    // link as a folder that is target (a junction: no rights needed); false when it cannot be made
    virtual bool LinkFolder(const std::filesystem::path& link, const std::filesystem::path& target, std::string& error) = 0;
    // link as a hard link to target (the same drive only); false when it cannot be made
    virtual bool LinkFile(const std::filesystem::path& link, const std::filesystem::path& target) = 0;
    // starts a program without waiting for it
    virtual bool Start(const std::filesystem::path& exe, const std::filesystem::path& cwd, std::string& error) = 0;
    virtual std::string ExeSuffix() const = 0;  // ".exe" on Windows
    virtual char PathSeparator() const = 0;     // between PATH entries
    virtual std::string GetEnv(const std::string& name) const = 0;
};

std::unique_ptr<Platform> MakePlatform();

}  // namespace launcher
