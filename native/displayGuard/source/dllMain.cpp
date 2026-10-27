#include "displayReport.h"
#include "focusReport.h"
#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"
#include "toolHost.h"
#include "windowReport.h"

#include <windows.h>

#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

static const char* const toolName = "displayGuard";
static const char* const displayGuardVersion = "0.1.0";

static bool isLoaded = false;

extern "C" {

// fixLoader calls this on the game's main thread at its entry point, before any game code runs. The tool
// has no Unload, so it loads only at game start. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", displayGuardVersion, windowFixesBuildCommit,
    GetCurrentThreadId());
  initializeHookRegistry();
  writeDisplayReport();
  installWindowReporting(host->mainThreadID);
  installFocusReporting(host->mainThreadID);
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
