// the log's opening lines: what the game runs on and with which settings
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <sys/sysctl.h>
#include <sys/utsname.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

#ifdef _WIN32
#pragma comment(lib, "advapi32.lib")
#endif

extern char** environ;

namespace app {

namespace {

#ifdef _WIN32
std::string RegString(HKEY key, const char* name) {
    char text[256] = {};
    DWORD size = sizeof(text) - 1;
    if (RegQueryValueExA(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(text), &size) != ERROR_SUCCESS) return {};
    return text;
}

std::string OsVersion() {
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS)
        return "unknown";
    std::string name = RegString(key, "ProductName"), display = RegString(key, "DisplayVersion"),
                build = RegString(key, "CurrentBuildNumber");
    DWORD ubr = 0, size = sizeof(ubr);
    RegQueryValueExA(key, "UBR", nullptr, nullptr, reinterpret_cast<LPBYTE>(&ubr), &size);
    RegCloseKey(key);
    // Windows 11 still names itself Windows 10 here, its build number tells
    if (auto ten = name.find("Windows 10"); ten != std::string::npos && std::atoi(build.c_str()) >= 22000)
        name.replace(ten, 10, "Windows 11");
    if (!display.empty()) name += " " + display;
    if (!build.empty()) name += " (build " + build + "." + std::to_string(ubr) + ")";
    using WineVersion = const char*(__cdecl*)();
    if (auto wine = reinterpret_cast<WineVersion>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))))
        name += std::string(" on Wine ") + wine();
    return name;
}

std::string CpuName() {
    int r[4] = {};
    __cpuid(r, int(0x80000000));
    if (uint32_t(r[0]) < 0x80000004) return "unknown";
    char brand[49] = {};
    for (int i = 0; i < 3; i++) {
        __cpuid(r, int(0x80000002 + i));
        std::memcpy(brand + i * 16, r, 16);
    }
    std::string s = brand;
    s.erase(0, s.find_first_not_of(' '));
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}
#else
std::string Sysctl(const char* name) {
    char text[256] = {};
    size_t size = sizeof(text) - 1;
    return sysctlbyname(name, text, &size, nullptr, 0) == 0 ? text : "unknown";
}
#endif

}  // namespace

#ifndef _WIN32
void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s", kGame.title, IMAGE_XBE, date);
    LOGR("Path: %s", ExeDir().string().c_str());
    utsname u{};
    uname(&u);
    LOGR("OS: macOS %s (%s %s, %s)", Sysctl("kern.osproductversion").c_str(), u.sysname, u.release, u.machine);
    int threads = 0;
    size_t size = sizeof(threads);
    sysctlbyname("hw.logicalcpu", &threads, &size, nullptr, 0);
    LOGR("CPU: %s (%d threads)", Sysctl("machdep.cpu.brand_string").c_str(), threads);
    uint64_t ram = 0;
    size = sizeof(ram);
    sysctlbyname("hw.memsize", &ram, &size, nullptr, 0);
    LOGR("RAM: %.1f GB", ram / 1073741824.0);
    Mode desk = Desktop();
    LOGR("Desktop: %ux%u", desk.width, desk.height);
    LogSettings();
    std::string game;
    for (const char* c = kGame.log; *c; c++) game += char(std::toupper(static_cast<unsigned char>(*c)));
    game += "_";
    for (char** e = environ; e && *e; e++)
        if (std::strncmp(*e, "XBE_", 4) == 0 || std::strncmp(*e, game.c_str(), game.size()) == 0) LOGR("Environment: %s", *e);
}
#else
void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s", kGame.title, IMAGE_XBE, date);
    LOGR("Path: %ls", ExeDir().c_str());
    LOGR("OS: %s", OsVersion().c_str());
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    LOGR("CPU: %s (%lu threads)", CpuName().c_str(), si.dwNumberOfProcessors);
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    LOGR("RAM: %.1f GB", mem.ullTotalPhys / 1073741824.0);
    // one line per adapter, each monitor reports its own
    std::vector<std::string> seen;
    for (DWORD i = 0;; i++) {
        DISPLAY_DEVICEA dd{sizeof(dd)};
        if (!EnumDisplayDevicesA(nullptr, i, &dd, 0)) break;
        if (std::find(seen.begin(), seen.end(), dd.DeviceString) != seen.end()) continue;
        seen.emplace_back(dd.DeviceString);
        LOGR("GPU %zu: %s%s", seen.size(), dd.DeviceString, (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "");
    }
    Mode desk = Desktop();
    LOGR("Desktop: %ux%u", desk.width, desk.height);
    DEVMODEW display{};
    display.dmSize = sizeof(display);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &display)) LOGR("Display: %u Hz", unsigned(display.dmDisplayFrequency));
    LogSettings();
    // experiment and diagnostic switches stay set in a shell or the user's environment between runs: list them
    std::wstring game;
    for (const char* c = kGame.log; *c; c++) game += wchar_t(std::toupper(static_cast<unsigned char>(*c)));
    game += L"_";
    // the wide block: the Windows SDK names the narrow one GetEnvironmentStrings only without UNICODE
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* e = env; *e; e += std::wcslen(e) + 1)
            if (std::wcsncmp(e, L"XBE_", 4) == 0 || std::wcsncmp(e, game.c_str(), game.size()) == 0) LOGR("Environment: %ls", e);
        FreeEnvironmentStringsW(env);
    }
}

#endif

}  // namespace app
