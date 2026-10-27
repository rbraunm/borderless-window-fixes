#include "fatalError.h"
#include "sessionLog.h"
#include "toolHost.h"

#include <windows.h>

#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

static const char* const toolName = "configBypass";
static const char* const configBypassVersion = "0.1.0";
static const wchar_t* const configurationProgramName = L"JadeEmpireConfig.exe";
static const wchar_t* const gameProgramName = L"JadeEmpire.exe";

static bool isLoaded = false;

static std::string toUTF8(const wchar_t* text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    fatalError("WideCharToMultiByte failed (error %lu)", GetLastError());
  }
  std::string converted(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, &converted[0], size, nullptr, nullptr);
  converted.resize(static_cast<size_t>(size) - 1);
  return converted;
}

static std::wstring executablePath() {
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    fatalError("Could not resolve the executable path (error %lu)", GetLastError());
  }
  return path;
}

extern "C" {

// The configuration program (the launcher's first Play and its Configure button, and Steam's configuration launch
// option) offers only fixed 4:3 resolutions and scans the system first. In it, this starts the game and ends the
// program before it shows anything; in the game it does nothing. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", configBypassVersion, windowFixesBuildCommit,
    GetCurrentThreadId());
  const std::wstring programPath = executablePath();
  const size_t separator = programPath.find_last_of(L'\\');
  const std::wstring folder = programPath.substr(0, separator);
  const wchar_t* programName = programPath.c_str() + separator + 1;
  if (_wcsicmp(programName, configurationProgramName) != 0) {
    writeLogLine("INFO", "notConfigurationProgram executable=%s", toUTF8(programName).c_str());
    writeLogLine("INFO", "loadComplete");
    return 0;
  }

  // Only the program's name and arguments are logged: a launcher can put a login ticket on a command line, not this one.
  writeLogLine("INFO", "configurationProgramStarted commandLine=%s", toUTF8(GetCommandLineW()).c_str());
  const std::wstring gamePath = folder + L"\\" + gameProgramName;
  std::wstring commandLine = L"\"" + gamePath + L"\"";
  STARTUPINFOW startupInfo = {};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo = {};
  if (!CreateProcessW(gamePath.c_str(), &commandLine[0], nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startupInfo,
      &processInfo)) {
    fatalError("Could not start %s (error %lu)", toUTF8(gamePath.c_str()).c_str(), GetLastError());
  }
  writeLogLine("INFO", "gameStarted processID=%lu path=%s", processInfo.dwProcessId, toUTF8(gamePath.c_str()).c_str());
  CloseHandle(processInfo.hThread);
  CloseHandle(processInfo.hProcess);
  writeLogLine("INFO", "configurationProgramEnded");
  closeSessionLog();
  ExitProcess(0);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

}
