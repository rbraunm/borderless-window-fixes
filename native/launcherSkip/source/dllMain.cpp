#include "entryPointHook.h"
#include "fatalError.h"
#include "modulePath.h"
#include "sessionLog.h"

#include <windows.h>

#include <cstdio>
#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif
#ifndef windowFixesGameExecutable
#error "windowFixesGameExecutable must be defined by the build (see build.ps1)"
#endif
#ifndef windowFixesLauncherExecutable
#error "windowFixesLauncherExecutable must be defined by the build (see build.ps1)"
#endif
#if !defined(_M_IX86)
#error "launcherSkip's msimg32.dll stubs are x86 only"
#endif

static const char* const toolName = "launcherSkip";
static const char* const launcherSkipVersion = "0.1.0";
static const char* const toolsFolderName = "windowFixes";

// X(exportName) for every export of the system msimg32.dll.
#define msimg32Exports(X) \
  X(vSetDdrawflag) \
  X(AlphaBlend) \
  X(DllInitialize) \
  X(GradientFill) \
  X(TransparentBlt)

#define declareMissingExport(exportName) \
  extern "C" void missing##exportName() { \
    fatalError("The system msimg32.dll does not export " #exportName ", but a program called it"); \
  }
msimg32Exports(declareMissingExport)

#define declarePointer(exportName) \
  void* real##exportName = reinterpret_cast<void*>(&missing##exportName);
extern "C" {
msimg32Exports(declarePointer)
}

#define declareStub(exportName) \
  extern "C" __declspec(naked) void proxy##exportName() { \
    __asm { jmp dword ptr [real##exportName] } \
  }
msimg32Exports(declareStub)

static HMODULE skipModule = nullptr;

static void loadSystemMsimg32() {
  char systemDirectory[MAX_PATH];
  const UINT systemDirectoryLength = GetSystemDirectoryA(systemDirectory, sizeof(systemDirectory));
  if (systemDirectoryLength == 0 || systemDirectoryLength >= sizeof(systemDirectory)) {
    fatalError("Could not find the Windows system directory (error %lu)", GetLastError());
  }
  // A full path, so the loader picks the system copy rather than this DLL again.
  const std::string systemPath = std::string(systemDirectory) + "\\msimg32.dll";
  HMODULE system = LoadLibraryA(systemPath.c_str());
  if (system == nullptr) {
    fatalError("Could not load %s (error %lu)", systemPath.c_str(), GetLastError());
  }
#define resolvePointer(exportName) \
  if (FARPROC resolved = GetProcAddress(system, #exportName)) { \
    real##exportName = reinterpret_cast<void*>(resolved); \
  }
  msimg32Exports(resolvePointer)
#undef resolvePointer
}

static std::string executablePath() {
  char path[MAX_PATH];
  const DWORD length = GetModuleFileNameA(nullptr, path, sizeof(path));
  if (length == 0 || length >= sizeof(path)) {
    fatalError("Could not resolve the executable path (error %lu)", GetLastError());
  }
  return path;
}

// Runs at the launcher's entry point on its main thread, before any launcher code: starts the game beside it and ends
// the launcher.
static void skipLauncher() {
  const std::string folder = resolveModuleDirectory(skipModule);
  SYSTEMTIME now;
  GetLocalTime(&now);
  char sessionStamp[32];
  snprintf(sessionStamp, sizeof(sessionStamp), "%04u%02u%02u_%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
    now.wMinute, now.wSecond);
  openSessionLog(folder + "\\" + toolsFolderName, toolName, sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s launcher=%s", launcherSkipVersion, windowFixesBuildCommit,
    executablePath().c_str());
  const std::string gamePath = folder + "\\" + windowFixesGameExecutable;
  std::string commandLine = "\"" + gamePath + "\"";
  STARTUPINFOA startupInfo = {};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo = {};
  if (!CreateProcessA(gamePath.c_str(), &commandLine[0], nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startupInfo,
      &processInfo)) {
    fatalError("Could not start %s (error %lu)", gamePath.c_str(), GetLastError());
  }
  writeLogLine("INFO", "gameStarted processID=%lu path=%s", processInfo.dwProcessId, gamePath.c_str());
  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);
  writeLogLine("INFO", "launcherEnded");
  closeSessionLog();
  ExitProcess(0);
}

// The launcher imports msimg32.dll, so Windows loads this copy from the game's folder at its startup. In any other
// program it only passes calls through to the system copy.
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    setFatalErrorTitle(toolName);
    skipModule = instance;
    DisableThreadLibraryCalls(instance);
    loadSystemMsimg32();
    const std::string path = executablePath();
    const size_t separator = path.find_last_of('\\');
    if (_stricmp(path.c_str() + separator + 1, windowFixesLauncherExecutable) == 0 && reserved != nullptr) {
      installEntryPointHook(&skipLauncher);
    }
  }
  return TRUE;
}
