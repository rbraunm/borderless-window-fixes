#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"
#include "settingsFile.h"
#include "toolHost.h"

#include <windows.h>

#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

typedef HWND (WINAPI* CreateWindowExWFunction)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef HWND (WINAPI* CreateWindowExAFunction)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI* SetWindowPosFunction)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI* MoveWindowFunction)(HWND, int, int, int, int, BOOL);

static const char* const toolName = "borderlessFit";
static const char* const borderlessFitVersion = "0.1.0";
static const char* const configurationFileName = "borderlessFit.cfg";

static bool isLoaded = false;
static DWORD gameMainThreadID = 0;
static int heightAdjustment = 0;
static CreateWindowExWFunction originalCreateWindowExW = nullptr;
static CreateWindowExAFunction originalCreateWindowExA = nullptr;
static SetWindowPosFunction originalSetWindowPos = nullptr;
static MoveWindowFunction originalMoveWindow = nullptr;

struct MonitorSearch {
  RECT requested;
  bool isMonitorRect;
};

static BOOL CALLBACK matchMonitor(HMONITOR, HDC, LPRECT bounds, LPARAM searchAddress) {
  MonitorSearch* search = reinterpret_cast<MonitorSearch*>(searchAddress);
  if (EqualRect(bounds, &search->requested)) {
    search->isMonitorRect = true;
    return FALSE;
  }
  return TRUE;
}

// The main thread's popup windows asking for exactly a monitor's rectangle are the borderless game window.
static void fitHeight(const char* function, DWORD style, HWND parent, int x, int y, int width, int* height) {
  if (heightAdjustment == 0 || GetCurrentThreadId() != gameMainThreadID || parent != nullptr || (style & WS_POPUP) == 0 ||
      (style & WS_CHILD) != 0) {
    return;
  }
  MonitorSearch search = { { x, y, x + width, y + *height }, false };
  EnumDisplayMonitors(nullptr, nullptr, &matchMonitor, reinterpret_cast<LPARAM>(&search));
  if (!search.isMonitorRect) {
    return;
  }
  writeLogLine("INFO", "windowFitted function=%s x=%d y=%d width=%d askedHeight=%d height=%d", function, x, y, width, *height,
    *height + heightAdjustment);
  *height += heightAdjustment;
}

static HWND WINAPI createWindowExWDetour(DWORD exStyle, LPCWSTR className, LPCWSTR title, DWORD style, int x, int y, int width,
    int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
  fitHeight("CreateWindowExW", style, parent, x, y, width, &height);
  return originalCreateWindowExW(exStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
}

static HWND WINAPI createWindowExADetour(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y, int width,
    int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID parameter) {
  fitHeight("CreateWindowExA", style, parent, x, y, width, &height);
  return originalCreateWindowExA(exStyle, className, title, style, x, y, width, height, parent, menu, instance, parameter);
}

static bool isTopLevel(HWND window) {
  return window != nullptr && GetAncestor(window, GA_ROOT) == window;
}

// SDL resizes without moving (SWP_NOMOVE), so the window's current position stands in for the ignored x and y.
static BOOL WINAPI setWindowPosDetour(HWND window, HWND insertAfter, int x, int y, int width, int height, UINT flags) {
  if ((flags & SWP_NOSIZE) == 0 && isTopLevel(window)) {
    POINT position = { x, y };
    if ((flags & SWP_NOMOVE) != 0) {
      RECT bounds = {};
      if (!GetWindowRect(window, &bounds)) {
        fatalError("GetWindowRect failed for window %p (error %lu)", static_cast<void*>(window), GetLastError());
      }
      position = { bounds.left, bounds.top };
    }
    fitHeight("SetWindowPos", static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), nullptr, position.x, position.y, width,
      &height);
  }
  return originalSetWindowPos(window, insertAfter, x, y, width, height, flags);
}

static BOOL WINAPI moveWindowDetour(HWND window, int x, int y, int width, int height, BOOL repaint) {
  if (isTopLevel(window)) {
    fitHeight("MoveWindow", static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), nullptr, x, y, width, &height);
  }
  return originalMoveWindow(window, x, y, width, height, repaint);
}

template <typename Function>
static void hookUser32(const char* exportName, Function detour, Function* original) {
  installExportHook("user32.dll", exportName, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

extern "C" {

// fixLoader calls this on the game's main thread at its entry point, before any game code runs and so before the game
// creates its window. The tool has no Unload, so it loads only at game start. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", borderlessFitVersion, windowFixesBuildCommit,
    GetCurrentThreadId());
  std::string adjustment;
  const SettingDefinition definitions[] = {
    { "heightAdjustment", SettingKind::text, &adjustment, false },
  };
  loadSettingsFile(host->toolDirectory, configurationFileName, definitions, sizeof(definitions) / sizeof(definitions[0]));
  if (adjustment == "taller") {
    heightAdjustment = 1;
  } else if (adjustment == "shorter") {
    heightAdjustment = -1;
  } else if (adjustment != "none") {
    fatalError("%s: heightAdjustment must be taller, shorter or none, not %s", configurationFileName, adjustment.c_str());
  }
  writeLogLine("INFO", "configurationLoaded heightAdjustment=%s", adjustment.c_str());

  gameMainThreadID = host->mainThreadID;
  initializeHookRegistry();
  hookUser32("CreateWindowExW", &createWindowExWDetour, &originalCreateWindowExW);
  hookUser32("CreateWindowExA", &createWindowExADetour, &originalCreateWindowExA);
  hookUser32("SetWindowPos", &setWindowPosDetour, &originalSetWindowPos);
  hookUser32("MoveWindow", &moveWindowDetour, &originalMoveWindow);
  writeLogLine("INFO", "loadComplete");
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

}
