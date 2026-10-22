// The launcher core's platform on Linux, for its test tool: programs through fork and exec, downloads through the
// curl program. .msi files cannot be extracted here.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/net.h"
#include "core/platform.h"

extern char** environ;

namespace launcher {

namespace {

class PosixPlatform : public Platform {
public:
    int Run(const std::vector<std::string>& args, const std::filesystem::path& cwd,
            const std::map<std::string, std::string>& env, const std::function<void(const std::string&)>& line,
            const std::atomic<bool>* cancel) override {
        int fd[2];
        if (args.empty() || pipe(fd) != 0) return -1;
        pid_t pid = fork();
        if (pid == 0) {
            dup2(fd[1], 1);
            dup2(fd[1], 2);
            close(fd[0]);
            close(fd[1]);
            if (chdir(cwd.c_str()) != 0) _exit(127);
            for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
            std::vector<char*> argv;
            for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        close(fd[1]);
        std::string pending;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd[0], buf, sizeof(buf))) > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    if (!pending.empty() && line) line(pending);
                    pending.clear();
                } else {
                    pending += buf[i];
                }
            }
            if (cancel && cancel->load()) kill(pid, SIGTERM);
        }
        if (!pending.empty() && line) line(pending);
        close(fd[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        if (cancel && cancel->load()) return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    bool ExtractMsi(const std::filesystem::path& msi, const std::filesystem::path&, std::string& error) override {
        error = "no .msi extraction on Linux (" + msi.filename().string() + ")";
        return false;
    }
    bool LinkFolder(const std::filesystem::path& link, const std::filesystem::path& target, std::string& error) override {
        std::error_code ec;
        std::filesystem::create_directory_symlink(target, link, ec);
        if (ec) error = ec.message();
        return !ec;
    }
    bool LinkFile(const std::filesystem::path& link, const std::filesystem::path& target) override {
        std::error_code ec;
        std::filesystem::create_hard_link(target, link, ec);
        return !ec;
    }
    bool Start(const std::filesystem::path& exe, const std::filesystem::path& cwd, std::string& error) override {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            if (chdir(cwd.c_str()) != 0) _exit(127);
            execl(exe.c_str(), exe.c_str(), nullptr);
            _exit(127);
        }
        if (pid < 0) error = "fork failed";
        return pid > 0;
    }
    std::string ExeSuffix() const override { return ""; }
    char PathSeparator() const override { return ':'; }
    std::string GetEnv(const std::string& name) const override {
        const char* v = std::getenv(name.c_str());
        return v ? v : "";
    }
};

class CurlHttp : public Http {
public:
    bool Get(const std::string& url, uint64_t offset, const std::function<bool(const uint8_t*, size_t)>& sink,
             uint64_t* total, std::string& error) override {
        std::string cmd = "curl -sSLf --retry 2 ";
        if (offset) cmd += "-r " + std::to_string(offset) + "- ";
        cmd += "'" + url + "'";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) {
            error = "cannot run curl";
            return false;
        }
        std::vector<uint8_t> buf(1 << 16);
        size_t n;
        bool go = true;
        while (go && (n = fread(buf.data(), 1, buf.size(), p)) > 0) go = sink(buf.data(), n);
        int status = pclose(p);
        if (total && *total == 0) *total = 0;
        if (go && status != 0) {
            error = "curl failed for " + url;
            return false;
        }
        return go;
    }
};

}  // namespace

std::unique_ptr<Platform> MakePlatform() { return std::make_unique<PosixPlatform>(); }
std::unique_ptr<Http> MakeHttp() { return std::make_unique<CurlHttp>(); }

}  // namespace launcher
