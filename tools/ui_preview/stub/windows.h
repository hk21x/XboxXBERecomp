// just what runtime/app/overlay.cpp asks of Windows, so it compiles on Linux for tools/ui_preview
#pragma once
#include <cstdint>
#include <cwchar>
typedef uint32_t DWORD, UINT;
typedef void* HMONITOR;
#define MAX_PATH 260
#define VK_ESCAPE 0x1B
struct MONITORINFOEXW { DWORD cbSize; wchar_t szDevice[32]; };
struct DEVMODEW { DWORD dmSize, dmBitsPerPel, dmPelsWidth, dmPelsHeight; };
bool GetMonitorInfoW(HMONITOR, MONITORINFOEXW*);
bool EnumDisplaySettingsW(const wchar_t*, DWORD, DEVMODEW*);
UINT GetWindowsDirectoryW(wchar_t*, UINT);
