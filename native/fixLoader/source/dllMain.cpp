#include "entryPointHook.h"
#include "fatalError.h"
#include "hookRegistry.h"
#include "lateStart.h"
#include "linePipe.h"
#include "modulePath.h"
#include "pumpHooks.h"
#include "sessionLog.h"
#include "toolHost.h"
#include "toolLoading.h"
#include "versionProxy.h"

#include <windows.h>

#include <cstdio>
#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

static const char* const toolName = "fixLoader";
static const char* const loaderVersion = "0.1.0";
// Everything but version.dll lives here: the tool list, the tools with their settings and every log.
// version.dll itself has to sit beside the game's executable, the only place the game picks it up from.
static const char* const toolsFolderName = "windowFixes";

static HMODULE loaderModule = nullptr;
// Names this game session in every tool's log file; tools reloaded later keep using it.
static char sessionStamp[32];
static LinePipeServer commandPipe;
// Loaded by LoadLibrary after the entry point ran, as a packed executable does while it unpacks.
static bool isLateStart = false;

static void requireDirectory(const std::string& path, const char* description) {
  const DWORD attributes = GetFileAttributesA(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    fatalError("The %s folder is missing: %s", description, path.c_str());
  }
}

static void logExecutable() {
  char executablePath[MAX_PATH];
  const DWORD executablePathLength = GetModuleFileNameA(nullptr, executablePath, sizeof(executablePath));
  if (executablePathLength == 0 || executablePathLength >= sizeof(executablePath)) {
    fatalError("Could not resolve the game executable path (error %lu)", GetLastError());
  }
  const BYTE* executableBase = reinterpret_cast<const BYTE*>(GetModuleHandleA(nullptr));
  const IMAGE_NT_HEADERS* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
    executableBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(executableBase)->e_lfanew);
  // Not the command line: a launcher may put a login ticket on it.
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu executable=%s timeDateStamp=0x%08lX "
    "sizeOfImage=0x%08lX imageBase=%p", loaderVersion, windowFixesBuildCommit, GetCurrentThreadId(), executablePath,
    headers->FileHeader.TimeDateStamp, headers->OptionalHeader.SizeOfImage, static_cast<const void*>(executableBase));
}

// Runs on the main thread outside the loader lock: at the game's entry point, or after a late load at the unpacked
// game's first startup call.
static void runLoader() {
  const std::string clientFolder = resolveModuleDirectory(loaderModule);
  const std::string toolsDirectory = clientFolder + "\\" + toolsFolderName;
  requireDirectory(toolsDirectory, toolsFolderName);
  SYSTEMTIME now;
  GetLocalTime(&now);
  snprintf(sessionStamp, sizeof(sessionStamp), "%04u%02u%02u_%02u%02u%02u", now.wYear, now.wMonth, now.wDay, now.wHour,
    now.wMinute, now.wSecond);
  openSessionLog(toolsDirectory, toolName, sessionStamp);
  logExecutable();
  if (isLateStart) {
    writeLogLine("INFO", "startMode mode=late trigger=%s", lateStartTrigger());
  } else {
    writeLogLine("INFO", "startMode mode=entryPoint");
    initializeHookRegistry();
  }
  installPumpHooks(GetCurrentThreadId());
  const ToolHost host = { toolsDirectory.c_str(), sessionStamp, GetCurrentThreadId(), &addPumpCallback,
    &removePumpCallback };
  loadListedTools(host);
  commandPipe.startForProcess(toolName, &runLoaderCommand);
  writeLogLine("INFO", "loadComplete");
}

// Every program in the game's folder that imports version.dll loads this copy: only the game's executable and its
// companions (game.cfg) get the tools.
static bool isGameExecutable() {
  char executablePath[MAX_PATH];
  const DWORD executablePathLength = GetModuleFileNameA(nullptr, executablePath, sizeof(executablePath));
  if (executablePathLength == 0 || executablePathLength >= sizeof(executablePath)) {
    fatalError("Could not resolve the executable path (error %lu)", GetLastError());
  }
  const char* executableName = strrchr(executablePath, '\\');
  executableName = executableName != nullptr ? executableName + 1 : executablePath;
  const char* candidate = windowFixesGameExecutables;
  while (*candidate != '\0') {
    const char* separator = strchr(candidate, '|');
    const size_t length = separator != nullptr ? static_cast<size_t>(separator - candidate) : strlen(candidate);
    if (strlen(executableName) == length && _strnicmp(executableName, candidate, length) == 0) {
      return true;
    }
    candidate += separator != nullptr ? length + 1 : length;
  }
  return false;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    setFatalErrorTitle(toolName);
    loaderModule = instance;
    DisableThreadLibraryCalls(instance);
    loadSystemVersionLibrary();
    if (!isGameExecutable()) {
      return TRUE;
    }
    // A null reserved pointer means LoadLibrary at runtime: the entry point has already run, so the tools start at the
    // unpacked game's first startup call instead.
    if (reserved == nullptr) {
      isLateStart = true;
      initializeHookRegistry();
      installLateStart(&runLoader);
    } else {
      installEntryPointHook(&runLoader);
    }
  }
  return TRUE;
}
