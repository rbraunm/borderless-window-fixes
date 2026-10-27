#include "configuration.h"
#include "crashReporter.h"
#include "exitGuard.h"
#include "fatalError.h"
#include "freezeWatchdog.h"
#include "hookRegistry.h"
#include "memoryDumps.h"
#include "processExit.h"
#include "sessionLog.h"
#include "toolHost.h"

#include <windows.h>

#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

static const char* const toolName = "stabilityGuard";
static const char* const stabilityGuardVersion = "0.1.0";

static bool isLoaded = false;

extern "C" {

// fixLoader calls this on the game's main thread at its entry point, before any game code runs,
// outside the loader lock. The tool has no Unload, so it loads only at game start. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  const std::string toolDirectory = host->toolDirectory;
  openSessionLog(toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", stabilityGuardVersion, windowFixesBuildCommit,
    GetCurrentThreadId());

  const Configuration configuration = loadConfiguration(toolDirectory);
  writeLogLine("INFO", "configurationLoaded maxFullReports=%lu writeMemoryDumps=%s maxDumpGigabytes=%llu stackScanSlots=%lu "
    "freezeSeconds=%lu exitGuard=%s exitGraceSeconds=%lu forceExitHotkey=%s", configuration.maxFullReports,
    configuration.writeMemoryDumps ? "true" : "false", configuration.maxDumpBytes / (1024ULL * 1024ULL * 1024ULL),
    configuration.stackScanSlots, configuration.freezeSeconds, configuration.exitGuard ? "true" : "false",
    configuration.exitGraceSeconds, configuration.forceExitHotkey ? "true" : "false");

  initializeHookRegistry();
  if (configuration.writeMemoryDumps) {
    startMemoryDumps(createLogFolder(toolDirectory, toolName), configuration.maxDumpBytes);
  }
  installCrashReporter(configuration, GetCurrentThreadId());
  installProcessExitLogging(configuration, GetCurrentThreadId());
  if (configuration.freezeSeconds > 0) {
    installFreezeWatchdog(configuration, GetCurrentThreadId());
  }
  if (configuration.exitGuard) {
    installExitGuard(configuration, GetCurrentThreadId());
  }
  writeLogLine("INFO", "loadComplete");
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  } else if (reason == DLL_PROCESS_DETACH && isLoaded) {
    writeCrashTotals();
  }
  return TRUE;
}

}
