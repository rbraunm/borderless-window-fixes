#include "fatalError.h"
#include "sessionLog.h"
#include "toolHost.h"

#include <windows.h>

#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

static const char* const toolName = "steamLocator";
static const char* const steamLocatorVersion = "0.1.0";
static const wchar_t* const steamKey = L"Software\\Valve\\Steam";
static const wchar_t* const steamPathValue = L"SteamPath";

static bool isLoaded = false;

static std::string toUTF8(const std::wstring& text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    fatalError("WideCharToMultiByte failed (error %lu)", GetLastError());
  }
  std::string converted(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &converted[0], size, nullptr, nullptr);
  converted.resize(static_cast<size_t>(size) - 1);
  return converted;
}

static std::wstring readSteamFolder() {
  DWORD size = 0;
  LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, steamKey, steamPathValue, RRF_RT_REG_SZ, nullptr, nullptr, &size);
  if (status != ERROR_SUCCESS) {
    fatalError("Steam is not installed for this user: HKEY_CURRENT_USER\\Software\\Valve\\Steam has no SteamPath (error %ld)",
      status);
  }
  std::wstring folder(size / sizeof(wchar_t), L'\0');
  status = RegGetValueW(HKEY_CURRENT_USER, steamKey, steamPathValue, RRF_RT_REG_SZ, nullptr, &folder[0], &size);
  if (status != ERROR_SUCCESS) {
    fatalError("Could not read HKEY_CURRENT_USER\\Software\\Valve\\Steam\\SteamPath (error %ld)", status);
  }
  folder.resize(wcslen(folder.c_str()));
  for (wchar_t& character : folder) {
    character = character == L'/' ? L'\\' : character;
  }
  return folder;
}

extern "C" {

// The game finds steam.dll only beside itself, so a game installed in a Steam library on another drive than Steam
// fails with "Failed to find Steam." Loading Steam's own copy first makes the game's LoadLibrary("steam.dll") return
// it, since Windows hands back a loaded DLL of the same name. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", steamLocatorVersion,
    windowFixesBuildCommit, GetCurrentThreadId());
  const std::wstring steamLibraryPath = readSteamFolder() + L"\\steam.dll";
  HMODULE steamLibrary = LoadLibraryExW(steamLibraryPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (steamLibrary == nullptr) {
    fatalError("Could not load %s (error %lu)", toUTF8(steamLibraryPath).c_str(), GetLastError());
  }
  writeLogLine("INFO", "steamLibraryLoaded path=%s base=%p", toUTF8(steamLibraryPath).c_str(),
    static_cast<void*>(steamLibrary));
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
