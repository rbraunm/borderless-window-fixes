#include "monitorWatch.h"

#include "fatalError.h"
#include "sessionLog.h"

#include <windows.h>

#include <cstring>

namespace {

const DWORD sampleMilliseconds = 250;
const DWORD monitorCapacity = 16;

struct MonitorMode {
  char device[CCHDEVICENAME];
  DWORD width;
  DWORD height;
  DWORD bits;
  DWORD hertz;
};

MonitorMode loggedModes[monitorCapacity];
DWORD loggedModeCount = 0;

bool isGameInForeground() {
  DWORD processID = 0;
  HWND foreground = GetForegroundWindow();
  return foreground != nullptr && GetWindowThreadProcessId(foreground, &processID) != 0 && processID == GetCurrentProcessId();
}

MonitorMode* findLoggedMode(const char* device) {
  for (DWORD index = 0; index < loggedModeCount; index++) {
    if (strcmp(loggedModes[index].device, device) == 0) {
      return &loggedModes[index];
    }
  }
  return nullptr;
}

void sampleMonitors() {
  DISPLAY_DEVICEA adapter = {};
  adapter.cb = sizeof(adapter);
  for (DWORD adapterIndex = 0; EnumDisplayDevicesA(nullptr, adapterIndex, &adapter, 0); adapterIndex++) {
    if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) {
      continue;
    }
    DEVMODEA current = {};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsA(adapter.DeviceName, ENUM_CURRENT_SETTINGS, &current)) {
      continue;
    }
    MonitorMode* logged = findLoggedMode(adapter.DeviceName);
    const bool isNew = logged == nullptr;
    if (isNew) {
      if (loggedModeCount == monitorCapacity) {
        fatalError("More than %lu monitors; raise monitorCapacity", monitorCapacity);
      }
      logged = &loggedModes[loggedModeCount++];
      strncpy(logged->device, adapter.DeviceName, sizeof(logged->device));
      logged->device[sizeof(logged->device) - 1] = '\0';
    } else if (logged->width == current.dmPelsWidth && logged->height == current.dmPelsHeight && logged->bits == current.dmBitsPerPel &&
        logged->hertz == current.dmDisplayFrequency) {
      continue;
    }
    writeLogLine("INFO", "monitorMode device=%s change=%s width=%lu height=%lu bits=%lu hertz=%lu previousHertz=%lu foreground=%s",
      adapter.DeviceName, isNew ? "first" : "changed", current.dmPelsWidth, current.dmPelsHeight, current.dmBitsPerPel,
      current.dmDisplayFrequency, isNew ? 0 : logged->hertz, isGameInForeground() ? "true" : "false");
    logged->width = current.dmPelsWidth;
    logged->height = current.dmPelsHeight;
    logged->bits = current.dmBitsPerPel;
    logged->hertz = current.dmDisplayFrequency;
  }
}

DWORD WINAPI monitorWatchThread(LPVOID) {
  for (;;) {
    sampleMonitors();
    Sleep(sampleMilliseconds);
  }
}

}

void startMonitorWatch() {
  sampleMonitors();
  HANDLE thread = CreateThread(nullptr, 0, &monitorWatchThread, nullptr, 0, nullptr);
  if (thread == nullptr) {
    fatalError("Could not start the monitor watch thread (error %lu)", GetLastError());
  }
  CloseHandle(thread);
}
