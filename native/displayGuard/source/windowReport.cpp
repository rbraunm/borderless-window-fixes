#include "windowReport.h"

#include "hookRegistry.h"
#include "sessionLog.h"

#include <cstring>
#include <string>

namespace {

typedef HWND (WINAPI* CreateWindowExWFunction)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef HWND (WINAPI* CreateWindowExAFunction)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI* SetWindowPosFunction)(HWND, HWND, int, int, int, int, UINT);
typedef LONG_PTR (WINAPI* SetWindowLongPtrWFunction)(HWND, int, LONG_PTR);
typedef LONG_PTR (WINAPI* SetWindowLongPtrAFunction)(HWND, int, LONG_PTR);
typedef LONG (WINAPI* ChangeDisplaySettingsExWFunction)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
typedef LONG (WINAPI* ChangeDisplaySettingsExAFunction)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
typedef HMODULE (WINAPI* LoadLibraryExWFunction)(LPCWSTR, HANDLE, DWORD);

DWORD gameMainThreadID = 0;
CreateWindowExWFunction originalCreateWindowExW = nullptr;
CreateWindowExAFunction originalCreateWindowExA = nullptr;
SetWindowPosFunction originalSetWindowPos = nullptr;
SetWindowLongPtrWFunction originalSetWindowLongPtrW = nullptr;
SetWindowLongPtrAFunction originalSetWindowLongPtrA = nullptr;
ChangeDisplaySettingsExWFunction originalChangeDisplaySettingsExW = nullptr;
ChangeDisplaySettingsExAFunction originalChangeDisplaySettingsExA = nullptr;
LoadLibraryExWFunction originalLoadLibraryExW = nullptr;

bool isMainThread() {
  return GetCurrentThreadId() == gameMainThreadID;
}

bool isTopLevel(HWND window) {
  return window != nullptr && GetAncestor(window, GA_ROOT) == window;
}

// The Windows ANSI code page, which the log is written in.
std::string narrow(const wchar_t* text) {
  if (text == nullptr) {
    return "";
  }
  // A class given as an atom is a small integer, not a string.
  if (reinterpret_cast<uintptr_t>(text) < 0x10000) {
    return "atom" + std::to_string(reinterpret_cast<uintptr_t>(text));
  }
  const int length = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (length <= 0) {
    return "";
  }
  std::string converted(static_cast<size_t>(length) - 1, '\0');
  WideCharToMultiByte(CP_ACP, 0, text, -1, &converted[0], length, nullptr, nullptr);
  return converted;
}

std::string narrow(const char* text) {
  if (text == nullptr) {
    return "";
  }
  if (reinterpret_cast<uintptr_t>(text) < 0x10000) {
    return "atom" + std::to_string(reinterpret_cast<uintptr_t>(text));
  }
  return text;
}

std::string windowClass(HWND window) {
  char className[128];
  return GetClassNameA(window, className, sizeof(className)) > 0 ? className : "";
}

const char* monitorOf(HWND window, char* buffer, size_t bufferSize) {
  MONITORINFOEXA information = {};
  information.cbSize = sizeof(information);
  HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
  if (monitor == nullptr || !GetMonitorInfoA(monitor, &information)) {
    strncpy(buffer, "none", bufferSize);
  } else {
    strncpy(buffer, information.szDevice, bufferSize);
  }
  buffer[bufferSize - 1] = '\0';
  return buffer;
}

void logWindowCreated(HWND window, const std::string& className, const std::string& title, DWORD style, DWORD exStyle,
    int x, int y, int width, int height, HWND parent) {
  if (window == nullptr || !isMainThread() || parent != nullptr) {
    return;
  }
  RECT bounds = {};
  GetWindowRect(window, &bounds);
  char monitor[CCHDEVICENAME];
  writeLogLine("INFO", "windowCreated window=%p class=%s title=%s style=0x%08lX exStyle=0x%08lX askedX=%d askedY=%d askedWidth=%d "
    "askedHeight=%d left=%ld top=%ld width=%ld height=%ld monitor=%s", static_cast<void*>(window), className.c_str(), title.c_str(),
    style, exStyle, x, y, width, height, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
    monitorOf(window, monitor, sizeof(monitor)));
}

HWND WINAPI createWindowExWDetour(DWORD exStyle, LPCWSTR className, LPCWSTR title, DWORD style, int x, int y, int width, int height,
    HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
  HWND window = originalCreateWindowExW(exStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
  logWindowCreated(window, narrow(className), narrow(title), style, exStyle, x, y, width, height, parent);
  return window;
}

HWND WINAPI createWindowExADetour(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int width, int height,
    HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
  HWND window = originalCreateWindowExA(exStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
  logWindowCreated(window, narrow(className), narrow(title), style, exStyle, x, y, width, height, parent);
  return window;
}

BOOL WINAPI setWindowPosDetour(HWND window, HWND insertAfter, int x, int y, int width, int height, UINT flags) {
  const BOOL result = originalSetWindowPos(window, insertAfter, x, y, width, height, flags);
  if (isMainThread() && isTopLevel(window)) {
    RECT bounds = {};
    GetWindowRect(window, &bounds);
    char monitor[CCHDEVICENAME];
    writeLogLine("INFO", "windowPlaced window=%p class=%s insertAfter=%p x=%d y=%d width=%d height=%d flags=0x%04X left=%ld top=%ld "
      "nowWidth=%ld nowHeight=%ld monitor=%s", static_cast<void*>(window), windowClass(window).c_str(), static_cast<void*>(insertAfter),
      x, y, width, height, flags, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
      monitorOf(window, monitor, sizeof(monitor)));
  }
  return result;
}

void logStyleChange(HWND window, int index, LONG_PTR value, LONG_PTR previous) {
  if (!isMainThread() || !isTopLevel(window) || (index != GWL_STYLE && index != GWL_EXSTYLE)) {
    return;
  }
  writeLogLine("INFO", "windowStyled window=%p class=%s which=%s from=0x%08llX to=0x%08llX", static_cast<void*>(window),
    windowClass(window).c_str(), index == GWL_STYLE ? "style" : "exStyle", static_cast<unsigned long long>(previous),
    static_cast<unsigned long long>(value));
}

LONG_PTR WINAPI setWindowLongPtrWDetour(HWND window, int index, LONG_PTR value) {
  const LONG_PTR previous = originalSetWindowLongPtrW(window, index, value);
  logStyleChange(window, index, value, previous);
  return previous;
}

LONG_PTR WINAPI setWindowLongPtrADetour(HWND window, int index, LONG_PTR value) {
  const LONG_PTR previous = originalSetWindowLongPtrA(window, index, value);
  logStyleChange(window, index, value, previous);
  return previous;
}

void logDisplayModeChange(const char* function, const std::string& device, const DEVMODEA* mode, DWORD flags, LONG result) {
  if (mode == nullptr) {
    writeLogLine("INFO", "displayModeChange function=%s device=%s mode=restore flags=0x%08lX result=%ld", function, device.c_str(), flags, result);
    return;
  }
  writeLogLine("INFO", "displayModeChange function=%s device=%s width=%lu height=%lu bits=%lu hertz=%lu fields=0x%08lX flags=0x%08lX "
    "result=%ld", function, device.c_str(), mode->dmPelsWidth, mode->dmPelsHeight, mode->dmBitsPerPel, mode->dmDisplayFrequency,
    mode->dmFields, flags, result);
}

LONG WINAPI changeDisplaySettingsExWDetour(LPCWSTR device, DEVMODEW* mode, HWND window, DWORD flags, LPVOID parameter) {
  const LONG result = originalChangeDisplaySettingsExW(device, mode, window, flags, parameter);
  if (mode == nullptr) {
    logDisplayModeChange("ChangeDisplaySettingsExW", narrow(device), nullptr, flags, result);
  } else {
    DEVMODEA narrowMode = {};
    narrowMode.dmPelsWidth = mode->dmPelsWidth;
    narrowMode.dmPelsHeight = mode->dmPelsHeight;
    narrowMode.dmBitsPerPel = mode->dmBitsPerPel;
    narrowMode.dmDisplayFrequency = mode->dmDisplayFrequency;
    narrowMode.dmFields = mode->dmFields;
    logDisplayModeChange("ChangeDisplaySettingsExW", narrow(device), &narrowMode, flags, result);
  }
  return result;
}

LONG WINAPI changeDisplaySettingsExADetour(LPCSTR device, DEVMODEA* mode, HWND window, DWORD flags, LPVOID parameter) {
  const LONG result = originalChangeDisplaySettingsExA(device, mode, window, flags, parameter);
  logDisplayModeChange("ChangeDisplaySettingsExA", narrow(device), mode, flags, result);
  return result;
}

const char* const graphicsLibraryNames[] = { "opengl32.dll", "d3d9.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "vulkan-1.dll" };

// Which graphics APIs the game brings in at runtime; one it imports is loaded before the tools.
HMODULE WINAPI loadLibraryExWDetour(LPCWSTR fileName, HANDLE file, DWORD flags) {
  HMODULE module = originalLoadLibraryExW(fileName, file, flags);
  const std::string name = narrow(fileName);
  const size_t nameStart = name.find_last_of("\\/");
  const std::string baseName = nameStart == std::string::npos ? name : name.substr(nameStart + 1);
  for (const char* graphicsLibraryName : graphicsLibraryNames) {
    if (_stricmp(baseName.c_str(), graphicsLibraryName) == 0) {
      writeLogLine("INFO", "graphicsLibraryLoaded file=%s loaded=%s error=%lu", name.c_str(), module != nullptr ? "true" : "false",
        module != nullptr ? 0 : GetLastError());
    }
  }
  return module;
}

}

// 32-bit user32 has no SetWindowLongPtr exports: the header maps them to SetWindowLong.
#if defined(_M_IX86)
#define setWindowLongWName "SetWindowLongW"
#define setWindowLongAName "SetWindowLongA"
#else
#define setWindowLongWName "SetWindowLongPtrW"
#define setWindowLongAName "SetWindowLongPtrA"
#endif

void installWindowReporting(DWORD mainThreadID) {
  gameMainThreadID = mainThreadID;
  installExportHook("user32.dll", "CreateWindowExW", reinterpret_cast<void*>(&createWindowExWDetour), reinterpret_cast<void**>(&originalCreateWindowExW));
  installExportHook("user32.dll", "CreateWindowExA", reinterpret_cast<void*>(&createWindowExADetour), reinterpret_cast<void**>(&originalCreateWindowExA));
  installExportHook("user32.dll", "SetWindowPos", reinterpret_cast<void*>(&setWindowPosDetour), reinterpret_cast<void**>(&originalSetWindowPos));
  installExportHook("user32.dll", setWindowLongWName, reinterpret_cast<void*>(&setWindowLongPtrWDetour), reinterpret_cast<void**>(&originalSetWindowLongPtrW));
  installExportHook("user32.dll", setWindowLongAName, reinterpret_cast<void*>(&setWindowLongPtrADetour), reinterpret_cast<void**>(&originalSetWindowLongPtrA));
  installExportHook("user32.dll", "ChangeDisplaySettingsExW", reinterpret_cast<void*>(&changeDisplaySettingsExWDetour),
    reinterpret_cast<void**>(&originalChangeDisplaySettingsExW));
  installExportHook("user32.dll", "ChangeDisplaySettingsExA", reinterpret_cast<void*>(&changeDisplaySettingsExADetour),
    reinterpret_cast<void**>(&originalChangeDisplaySettingsExA));
  // Every LoadLibrary variant ends in kernelbase's LoadLibraryExW; kernel32's export is only one way in.
  installExportHook("kernelbase.dll", "LoadLibraryExW", reinterpret_cast<void*>(&loadLibraryExWDetour), reinterpret_cast<void**>(&originalLoadLibraryExW));
}
