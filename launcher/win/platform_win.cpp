// The launcher's platform on Windows: WinHTTP downloads, programs in a job (cancel ends the whole tree: CMake, Ninja,
// clang), msiexec /a for the SDK's installers, junctions and hard links for the player's game
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <vector>

#include "core/net.h"
#include "core/platform.h"
#include "core/util.h"

namespace launcher {

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string LastError(const char* what) {
    DWORD e = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e,
                   0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string text = msg ? Narrow(msg) : std::string();
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return std::string(what) + " (" + std::to_string(e) + (text.empty() ? "" : ": " + text) + ")";
}

// one argument as the C runtime splits command lines: quoted when it has spaces, backslashes doubled before quotes
std::wstring Quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            slashes++;
        } else {
            if (c == L'"') q.append(slashes + 1, L'\\');
            slashes = 0;
        }
        q += c;
    }
    q.append(slashes, L'\\');
    return q + L"\"";
}

bool SameName(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towupper(x) == std::towupper(y);
           });
}

// the inherited environment with these entries replaced (names compare without case), as CreateProcess takes it
std::vector<wchar_t> EnvBlock(const std::map<std::string, std::string>& env) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring e = p;
            size_t eq = e.find(L'=', 1);  // "=C:=C:\..." entries start with '='
            if (eq == std::wstring::npos) continue;
            vars.push_back({e.substr(0, eq), e.substr(eq + 1)});
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [k, v] : env) {
        std::wstring wk = Wide(k);
        vars.erase(std::remove_if(vars.begin(), vars.end(), [&](const auto& kv) { return SameName(kv.first, wk); }), vars.end());
        vars.push_back({wk, Wide(v)});
    }
    std::sort(vars.begin(), vars.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    std::vector<wchar_t> out;
    for (const auto& [k, v] : vars) {
        std::wstring e = k + L"=" + v;
        out.insert(out.end(), e.begin(), e.end());
        out.push_back(0);
    }
    out.push_back(0);
    return out;
}

class WinPlatform : public Platform {
public:
    int Run(const std::vector<std::string>& args, const std::filesystem::path& cwd,
            const std::map<std::string, std::string>& env, const std::function<void(const std::string&)>& line,
            const std::atomic<bool>* cancel) override {
        std::wstring cmd;
        for (const std::string& a : args) cmd += (cmd.empty() ? L"" : L" ") + Quote(Wide(a));
        return RunLine(cmd, cwd, env, line, cancel);
    }

    int RunLine(std::wstring cmd, const std::filesystem::path& cwd, const std::map<std::string, std::string>& env,
                const std::function<void(const std::string&)>& line, const std::atomic<bool>* cancel) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &sa, 0)) return -1;
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = write;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> block = EnvBlock(env);
        // a job: closing it ends the program and every program it started
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
        std::wstring dir = cwd.wstring();
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, block.data(),
                                 dir.empty() ? nullptr : dir.c_str(), &si, &pi);
        CloseHandle(write);
        if (!ok) {
            if (line) line(LastError(("cannot start " + Narrow(cmd)).c_str()));
            CloseHandle(read);
            if (job) CloseHandle(job);
            return -1;
        }
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        // a watcher ends the job when cancelled (the read below blocks until output or the end)
        std::atomic<bool> done{false};
        HANDLE watch = nullptr;
        struct Watch {
            HANDLE job;
            const std::atomic<bool>* cancel;
            std::atomic<bool>* done;
        } w{job, cancel, &done};
        if (cancel && job)
            watch = CreateThread(nullptr, 0, [](void* p) -> DWORD {
                auto* w = static_cast<Watch*>(p);
                while (!w->done->load()) {
                    if (w->cancel->load()) {
                        TerminateJobObject(w->job, 1);
                        break;
                    }
                    Sleep(100);
                }
                return 0;
            }, &w, 0, nullptr);
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n) {
            for (DWORD i = 0; i < n; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    if (!pending.empty() && line) line(pending);
                    pending.clear();
                } else {
                    pending += buf[i];
                }
            }
        }
        if (!pending.empty() && line) line(pending);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        done = true;
        if (watch) {
            WaitForSingleObject(watch, INFINITE);
            CloseHandle(watch);
        }
        CloseHandle(pi.hProcess);
        CloseHandle(read);
        if (job) CloseHandle(job);
        if (cancel && cancel->load()) return -1;
        return int(code);
    }

    bool ExtractMsi(const std::filesystem::path& msi, const std::filesystem::path& target, std::string& error) override {
        // msiexec reads PROPERTY="value" itself, so this line is put together by hand
        std::filesystem::path log = target.parent_path() / (msi.stem().wstring() + L".msiexec.log");
        std::wstring dir = target.wstring();
        while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
        std::wstring cmd = L"msiexec.exe /a \"" + msi.wstring() + L"\" /qn /norestart TARGETDIR=\"" + dir +
                           L"\" /L*v \"" + log.wstring() + L"\"";
        std::string out;
        int code = RunLine(cmd, msi.parent_path(), {}, [&](const std::string& l) { out += l + "\n"; }, nullptr);
        if (code == 0 || code == 3010) return true;
        error = "Could not extract " + U8(msi.filename()) + " (msiexec " + std::to_string(code) + ", see " +
                U8(log) + ")";
        return false;
    }

    bool LinkFolder(const std::filesystem::path& link, const std::filesystem::path& target, std::string& error) override {
        // a junction (mklink /J): unlike a symbolic link it needs no rights
        std::wstring cmd = L"cmd.exe /c mklink /J \"" + link.wstring() + L"\" \"" + target.wstring() + L"\"";
        std::string out;
        int code = RunLine(cmd, link.parent_path(), {}, [&](const std::string& l) { out += l + " "; }, nullptr);
        if (code == 0) return true;
        error = "Could not link the game's folder: " + out;
        return false;
    }

    bool LinkFile(const std::filesystem::path& link, const std::filesystem::path& target) override {
        return CreateHardLinkW(link.c_str(), target.c_str(), nullptr) != 0;
    }

    bool Start(const std::filesystem::path& exe, const std::filesystem::path& cwd, std::string& error) override {
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        std::wstring cmd = Quote(exe.wstring());
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si, &pi)) {
            error = LastError("Could not start the game");
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    std::string ExeSuffix() const override { return ".exe"; }
    char PathSeparator() const override { return ';'; }
    std::string GetEnv(const std::string& name) const override {
        std::wstring n = Wide(name);
        DWORD len = GetEnvironmentVariableW(n.c_str(), nullptr, 0);
        if (!len) return {};
        std::wstring v(len, L'\0');
        v.resize(GetEnvironmentVariableW(n.c_str(), v.data(), len));
        return Narrow(v);
    }
};

class WinHttp : public Http {
public:
    WinHttp() {
        session_ = WinHttpOpen(L"XboxXBE-Recomp launcher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_)  // before Windows 8.1
            session_ = WinHttpOpen(L"XboxXBE-Recomp launcher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_) WinHttpSetTimeouts(session_, 15000, 15000, 30000, 60000);
    }
    ~WinHttp() override {
        if (session_) WinHttpCloseHandle(session_);
    }

    bool Get(const std::string& url, uint64_t offset, const std::function<bool(const uint8_t*, size_t)>& sink,
             uint64_t* total, std::string& error) override {
        if (!session_) {
            error = LastError("WinHTTP is unavailable");
            return false;
        }
        std::wstring wurl = Wide(url);
        URL_COMPONENTS uc{sizeof(uc)};
        wchar_t host[256], path[4096];
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path;
        uc.dwUrlPathLength = 4096;
        uc.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
            error = "bad address " + url;
            return false;
        }
        std::wstring object = std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength) +
                              (uc.lpszExtraInfo ? std::wstring(uc.lpszExtraInfo, uc.dwExtraInfoLength) : L"");
        HINTERNET conn = WinHttpConnect(session_, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
        if (!conn) {
            error = LastError("cannot reach the server");
            return false;
        }
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        bool ok = false;
        if (req) {
            std::wstring headers = offset ? L"Range: bytes=" + std::to_wstring(offset) + L"-\r\n" : L"";
            if (WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                   headers.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &size, WINHTTP_NO_HEADER_INDEX);
                wchar_t len[32] = {};
                DWORD lsize = sizeof(len);
                uint64_t length = WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                                      len, &lsize, WINHTTP_NO_HEADER_INDEX)
                                      ? _wcstoui64(len, nullptr, 10)
                                      : 0;
                // a server that ignores the range sends it all: the part already here is skipped
                uint64_t skip = (offset && status == 200) ? offset : 0;
                if (status == 200 || status == 206) {
                    if (total) *total = length ? (status == 206 ? offset + length : length) : 0;
                    ok = true;
                    std::vector<uint8_t> buf(1 << 16);
                    for (;;) {
                        DWORD got = 0;
                        if (!WinHttpReadData(req, buf.data(), DWORD(buf.size()), &got)) {
                            error = LastError("the download stopped");
                            ok = false;
                            break;
                        }
                        if (!got) break;
                        size_t from = 0;
                        if (skip) {
                            from = size_t(std::min<uint64_t>(skip, got));
                            skip -= from;
                        }
                        if (from < got && !sink(buf.data() + from, got - from)) {
                            ok = false;
                            error = "stopped";
                            break;
                        }
                    }
                } else {
                    error = "the server answered " + std::to_string(status) + " for " + url;
                }
            } else {
                error = LastError("the request failed");
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
        return ok;
    }

private:
    HINTERNET session_ = nullptr;
};

}  // namespace

std::unique_ptr<Platform> MakePlatform() { return std::make_unique<WinPlatform>(); }
std::unique_ptr<Http> MakeHttp() { return std::make_unique<WinHttp>(); }

}  // namespace launcher
