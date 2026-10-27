#include "fatalError.h"
#include "sessionLog.h"
#include "toolHost.h"

#include <windows.h>

#include <cstdio>
#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

static const char* const toolName = "renderSettings";
static const char* const renderSettingsVersion = "0.1.0";
static const char* const gameProgramName = "JadeEmpire.exe";
static const char* const settingsFileName = "JadeEmpire.ini";
static const char* const renderSection = "Render";
static const char* const gameSection = "Game";

static bool isLoaded = false;

static void writeSetting(const std::string& path, const char* section, const char* key, unsigned long value) {
  char previous[64];
  GetPrivateProfileStringA(section, key, "", previous, sizeof(previous), path.c_str());
  char text[32];
  snprintf(text, sizeof(text), "%lu", value);
  if (strcmp(previous, text) == 0) {
    return;
  }
  if (!WritePrivateProfileStringA(section, key, text, path.c_str())) {
    fatalError("Could not write %s [%s] %s (error %lu)", path.c_str(), section, key, GetLastError());
  }
  writeLogLine("INFO", "settingWritten section=%s key=%s from=%s to=%s", section, key, previous[0] != '\0' ? previous : "(none)",
    text);
}

extern "C" {

// The game reads JadeEmpire.ini before it creates its window. With d3d9Borderless its only display mode is the
// desktop's, so this sets the primary monitor's desktop mode in [Render] (the game draws on adapter 0), widescreen
// unless the monitor is 4:3, and turns the game's own 30 frames per second cap ([Game] ClampFPS) off, since
// d3d9Borderless's frameRateLimit holds the frame rate instead. In the configuration program it does nothing.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", renderSettingsVersion,
    windowFixesBuildCommit, GetCurrentThreadId());
  char executablePath[MAX_PATH];
  const DWORD length = GetModuleFileNameA(nullptr, executablePath, sizeof(executablePath));
  if (length == 0 || length >= sizeof(executablePath)) {
    fatalError("Could not resolve the executable path (error %lu)", GetLastError());
  }
  char* separator = strrchr(executablePath, '\\');
  if (_stricmp(separator + 1, gameProgramName) != 0) {
    writeLogLine("INFO", "notGameProgram executable=%s", separator + 1);
    writeLogLine("INFO", "loadComplete");
    return 0;
  }
  *separator = '\0';
  const std::string path = std::string(executablePath) + "\\" + settingsFileName;

  DEVMODEA desktop = {};
  desktop.dmSize = sizeof(desktop);
  if (!EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &desktop)) {
    fatalError("EnumDisplaySettings failed for the primary monitor");
  }
  writeLogLine("INFO", "desktopMode width=%lu height=%lu hertz=%lu", desktop.dmPelsWidth, desktop.dmPelsHeight,
    desktop.dmDisplayFrequency);
  writeSetting(path, renderSection, "ScrW", desktop.dmPelsWidth);
  writeSetting(path, renderSection, "ScrH", desktop.dmPelsHeight);
  writeSetting(path, renderSection, "RefreshRate", desktop.dmDisplayFrequency);
  writeSetting(path, renderSection, "Widescreen", desktop.dmPelsWidth * 3 == desktop.dmPelsHeight * 4 ? 0 : 1);
  writeSetting(path, renderSection, "Windowed", 0);
  writeSetting(path, gameSection, "ClampFPS", 0);
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
