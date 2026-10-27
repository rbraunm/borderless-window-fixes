#include "animationTiming.h"
#include "fatalError.h"
#include "gameClock.h"
#include "hookRegistry.h"
#include "physicsSteps.h"
#include "sessionLog.h"
#include "settingsFile.h"
#include "toolHost.h"

#include <windows.h>

#include <cstring>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

static const char* const toolName = "timeFixes";
static const char* const timeFixesVersion = "0.1.0";
static const char* const configurationFileName = "timeFixes.cfg";
static const char* const gameProgramName = "JadeEmpire.exe";

static bool isLoaded = false;
static bool isInstalled = false;
static unsigned long stepsPerSecond = 0;
static DWORD gameMainThreadID = 0;

// The code is still packed while the tools load; by the first message pump the game is running unpacked.
static void onPump() {
  if (isInstalled) {
    return;
  }
  isInstalled = true;
  installPreciseClock();
  installPhysicsSteps(stepsPerSecond, gameMainThreadID);
  installAnimationTiming();
  writeLogLine("INFO", "timeFixesInstalled");
}

extern "C" {

// Puts Jade Empire's timing on one game clock that never depends on the frame rate: a precise engine clock (Direct3D
// 9's single precision x87 rounds it to steps of up to 65 ms on a machine up for days, so the world updated in jumps),
// physics in fixed steps of game time, and the frame-stepped animation (life textures, scrolling meshes, wind) on game
// time. Game time is the world's when it runs, slow motion included, and real time while it is paused. In the
// configuration program it does nothing. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", timeFixesVersion, windowFixesBuildCommit,
    GetCurrentThreadId());
  char executablePath[MAX_PATH];
  const DWORD length = GetModuleFileNameA(nullptr, executablePath, sizeof(executablePath));
  if (length == 0 || length >= sizeof(executablePath)) {
    fatalError("Could not resolve the executable path (error %lu)", GetLastError());
  }
  const char* executableName = strrchr(executablePath, '\\') + 1;
  if (_stricmp(executableName, gameProgramName) != 0) {
    writeLogLine("INFO", "notGameProgram executable=%s", executableName);
    writeLogLine("INFO", "loadComplete");
    return 0;
  }
  const SettingDefinition definitions[] = {
    { "stepsPerSecond", SettingKind::positiveInteger, &stepsPerSecond, false },
  };
  loadSettingsFile(host->toolDirectory, configurationFileName, definitions, sizeof(definitions) / sizeof(definitions[0]));
  writeLogLine("INFO", "configurationLoaded stepsPerSecond=%lu", stepsPerSecond);
  gameMainThreadID = host->mainThreadID;
  initializeGameClock();
  initializeHookRegistry();
  host->addPumpCallback(&onPump);
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
