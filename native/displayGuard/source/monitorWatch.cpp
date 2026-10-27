#include "monitorWatch.h"

#include "fatalError.h"
#include "sessionLog.h"

#include <windows.h>

#include <cstring>
#include <vector>

namespace {

const DWORD sampleMilliseconds = 250;
const DWORD monitorCapacity = 16;

// What one monitor shows: the desktop mode Windows reports, the timing its link actually runs at, and whether
// advanced color (HDR) is on. A switch of any of them makes a monitor blank while it resynchronizes.
struct MonitorState {
  char device[CCHDEVICENAME];
  DWORD width;
  DWORD height;
  DWORD bits;
  DWORD hertz;
  UINT32 targetRefreshNumerator;
  UINT32 targetRefreshDenominator;
  int advancedColorSupported;
  int advancedColorEnabled;
};

MonitorState loggedStates[monitorCapacity];
DWORD loggedStateCount = 0;
bool hasLoggedQueryFailure = false;

bool isGameInForeground() {
  DWORD processID = 0;
  HWND foreground = GetForegroundWindow();
  return foreground != nullptr && GetWindowThreadProcessId(foreground, &processID) != 0 && processID == GetCurrentProcessId();
}

MonitorState* findLoggedState(const char* device) {
  for (DWORD index = 0; index < loggedStateCount; index++) {
    if (strcmp(loggedStates[index].device, device) == 0) {
      return &loggedStates[index];
    }
  }
  return nullptr;
}

bool isSameState(const MonitorState& first, const MonitorState& second) {
  return first.width == second.width && first.height == second.height && first.bits == second.bits && first.hertz == second.hertz &&
    first.targetRefreshNumerator == second.targetRefreshNumerator && first.targetRefreshDenominator == second.targetRefreshDenominator &&
    first.advancedColorSupported == second.advancedColorSupported && first.advancedColorEnabled == second.advancedColorEnabled;
}

bool readMonitorState(const DISPLAYCONFIG_PATH_INFO& path, MonitorState* state) {
  DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
  sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
  sourceName.header.size = sizeof(sourceName);
  sourceName.header.adapterId = path.sourceInfo.adapterId;
  sourceName.header.id = path.sourceInfo.id;
  if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
    return false;
  }
  *state = {};
  WideCharToMultiByte(CP_ACP, 0, sourceName.viewGdiDeviceName, -1, state->device, sizeof(state->device), nullptr, nullptr);
  DEVMODEA current = {};
  current.dmSize = sizeof(current);
  if (!EnumDisplaySettingsA(state->device, ENUM_CURRENT_SETTINGS, &current)) {
    return false;
  }
  state->width = current.dmPelsWidth;
  state->height = current.dmPelsHeight;
  state->bits = current.dmBitsPerPel;
  state->hertz = current.dmDisplayFrequency;
  state->targetRefreshNumerator = path.targetInfo.refreshRate.Numerator;
  state->targetRefreshDenominator = path.targetInfo.refreshRate.Denominator;
  DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
  color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
  color.header.size = sizeof(color);
  color.header.adapterId = path.targetInfo.adapterId;
  color.header.id = path.targetInfo.id;
  if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
    state->advancedColorSupported = color.advancedColorSupported;
    state->advancedColorEnabled = color.advancedColorEnabled;
  } else {
    state->advancedColorSupported = -1;
    state->advancedColorEnabled = -1;
  }
  return true;
}

void logState(const MonitorState& state, const char* change) {
  const double targetHertz = state.targetRefreshDenominator != 0
    ? static_cast<double>(state.targetRefreshNumerator) / state.targetRefreshDenominator : 0.0;
  writeLogLine("INFO", "monitorMode device=%s change=%s width=%lu height=%lu bits=%lu hertz=%lu targetHertz=%.3f advancedColorSupported=%d "
    "advancedColorEnabled=%d foreground=%s", state.device, change, state.width, state.height, state.bits, state.hertz, targetHertz,
    state.advancedColorSupported, state.advancedColorEnabled, isGameInForeground() ? "true" : "false");
}

void sampleMonitors() {
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  if (result == ERROR_SUCCESS) {
    paths.resize(pathCount);
    modes.resize(modeCount);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
  }
  // The configuration can change between the two calls while a monitor switches; the next sample tries again.
  if (result != ERROR_SUCCESS) {
    if (!hasLoggedQueryFailure) {
      hasLoggedQueryFailure = true;
      writeLogLine("WARN", "displayConfigUnreadable error=%ld foreground=%s", result, isGameInForeground() ? "true" : "false");
    }
    return;
  }
  hasLoggedQueryFailure = false;
  for (UINT32 index = 0; index < pathCount; index++) {
    MonitorState state;
    if (!readMonitorState(paths[index], &state)) {
      continue;
    }
    MonitorState* logged = findLoggedState(state.device);
    if (logged == nullptr) {
      if (loggedStateCount == monitorCapacity) {
        fatalError("More than %lu monitors; raise monitorCapacity", monitorCapacity);
      }
      loggedStates[loggedStateCount++] = state;
      logState(state, "first");
    } else if (!isSameState(*logged, state)) {
      *logged = state;
      logState(state, "changed");
    }
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
