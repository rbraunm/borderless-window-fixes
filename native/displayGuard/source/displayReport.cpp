#include "displayReport.h"

#include "sessionLog.h"

#include <windows.h>
#include <shellscalingapi.h>

#include <string>
#include <vector>

namespace {

typedef HRESULT (WINAPI* GetDpiForMonitorFunction)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
typedef DPI_AWARENESS_CONTEXT (WINAPI* GetThreadDpiAwarenessContextFunction)();
typedef DPI_AWARENESS (WINAPI* GetAwarenessFromDpiAwarenessContextFunction)(DPI_AWARENESS_CONTEXT);

BOOL CALLBACK reportMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM dpiFunction) {
  MONITORINFOEXA information = {};
  information.cbSize = sizeof(information);
  if (!GetMonitorInfoA(monitor, &information)) {
    writeLogLine("WARN", "monitorUnreadable error=%lu", GetLastError());
    return TRUE;
  }
  UINT dpiX = 0;
  UINT dpiY = 0;
  GetDpiForMonitorFunction getDpiForMonitor = reinterpret_cast<GetDpiForMonitorFunction>(dpiFunction);
  if (getDpiForMonitor != nullptr) {
    getDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  }
  DEVMODEA mode = {};
  mode.dmSize = sizeof(mode);
  const bool hasMode = EnumDisplaySettingsA(information.szDevice, ENUM_CURRENT_SETTINGS, &mode) != FALSE;
  writeLogLine("INFO", "monitor device=%s primary=%s left=%ld top=%ld width=%ld height=%ld workWidth=%ld workHeight=%ld dpi=%u "
    "modeWidth=%lu modeHeight=%lu modeHertz=%lu", information.szDevice, (information.dwFlags & MONITORINFOF_PRIMARY) != 0 ? "true" : "false",
    information.rcMonitor.left, information.rcMonitor.top, information.rcMonitor.right - information.rcMonitor.left,
    information.rcMonitor.bottom - information.rcMonitor.top, information.rcWork.right - information.rcWork.left,
    information.rcWork.bottom - information.rcWork.top, dpiX, hasMode ? mode.dmPelsWidth : 0, hasMode ? mode.dmPelsHeight : 0,
    hasMode ? mode.dmDisplayFrequency : 0);
  return TRUE;
}

void reportDpiAwareness() {
  HMODULE user32 = GetModuleHandleA("user32.dll");
  GetThreadDpiAwarenessContextFunction getContext = reinterpret_cast<GetThreadDpiAwarenessContextFunction>(
    reinterpret_cast<void*>(GetProcAddress(user32, "GetThreadDpiAwarenessContext")));
  GetAwarenessFromDpiAwarenessContextFunction getAwareness = reinterpret_cast<GetAwarenessFromDpiAwarenessContextFunction>(
    reinterpret_cast<void*>(GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext")));
  if (getContext == nullptr || getAwareness == nullptr) {
    writeLogLine("WARN", "dpiAwarenessUnavailable");
    return;
  }
  const DPI_AWARENESS awareness = getAwareness(getContext());
  const char* name = awareness == DPI_AWARENESS_UNAWARE ? "unaware" : awareness == DPI_AWARENESS_SYSTEM_AWARE ? "system"
    : awareness == DPI_AWARENESS_PER_MONITOR_AWARE ? "perMonitor" : "invalid";
  writeLogLine("INFO", "dpiAwareness process=%s systemDpi=%u", name, GetDpiForSystem());
}

}

void writeDisplayReport() {
  HMODULE shcore = LoadLibraryA("shcore.dll");
  GetDpiForMonitorFunction getDpiForMonitor = shcore != nullptr
    ? reinterpret_cast<GetDpiForMonitorFunction>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor"))) : nullptr;
  if (!EnumDisplayMonitors(nullptr, nullptr, &reportMonitor, reinterpret_cast<LPARAM>(getDpiForMonitor))) {
    writeLogLine("WARN", "monitorsUnreadable error=%lu", GetLastError());
  }
  reportDpiAwareness();
}
