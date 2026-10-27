#include "lateStart.h"

#include "fatalError.h"

#include <MinHook.h>

#include <windows.h>
#include <winternl.h>

typedef LPSTR (WINAPI* GetCommandLineAFunction)();
typedef LPWSTR (WINAPI* GetCommandLineWFunction)();
typedef VOID (WINAPI* GetStartupInfoAFunction)(LPSTARTUPINFOA);
typedef VOID (WINAPI* GetStartupInfoWFunction)(LPSTARTUPINFOW);
typedef DWORD (WINAPI* GetVersionFunction)();
typedef BOOL (WINAPI* GetVersionExAFunction)(LPOSVERSIONINFOA);
typedef HMODULE (WINAPI* LoadLibraryAFunction)(LPCSTR);
typedef HMODULE (WINAPI* LoadLibraryWFunction)(LPCWSTR);
typedef FARPROC (WINAPI* GetProcAddressFunction)(HMODULE, LPCSTR);

static const DWORD lateStartSeconds = 60;
#if defined(_M_X64)
static const size_t loaderLockOffset = 0x110;
#elif defined(_M_IX86)
static const size_t loaderLockOffset = 0xA0;
#endif

static void (*startCallback)() = nullptr;
static DWORD loadingThreadID = 0;
static volatile LONG isStarted = 0;
static const char* triggerName = nullptr;
static HANDLE startedEvent = nullptr;
static GetCommandLineAFunction originalGetCommandLineA = nullptr;
static GetCommandLineWFunction originalGetCommandLineW = nullptr;
static GetStartupInfoAFunction originalGetStartupInfoA = nullptr;
static GetStartupInfoWFunction originalGetStartupInfoW = nullptr;
static GetVersionFunction originalGetVersion = nullptr;
static GetVersionExAFunction originalGetVersionExA = nullptr;
static LoadLibraryAFunction originalLoadLibraryA = nullptr;
static LoadLibraryWFunction originalLoadLibraryW = nullptr;
static GetProcAddressFunction originalGetProcAddress = nullptr;

static bool isLoaderLockHeld() {
  const BYTE* peb = reinterpret_cast<const BYTE*>(NtCurrentTeb()->ProcessEnvironmentBlock);
  const RTL_CRITICAL_SECTION* loaderLock = *reinterpret_cast<RTL_CRITICAL_SECTION* const*>(peb + loaderLockOffset);
  return reinterpret_cast<DWORD_PTR>(loaderLock->OwningThread) == GetCurrentThreadId();
}

// The unpacker and the DLLs it loads make these calls too, but while loading DLLs, under the loader lock.
static void onStartupCall(const char* name) {
  if (isStarted != 0 || GetCurrentThreadId() != loadingThreadID || isLoaderLockHeld()) {
    return;
  }
  if (InterlockedExchange(&isStarted, 1) != 0) {
    return;
  }
  triggerName = name;
  SetEvent(startedEvent);
  startCallback();
}

static LPSTR WINAPI getCommandLineADetour() {
  onStartupCall("GetCommandLineA");
  return originalGetCommandLineA();
}

static LPWSTR WINAPI getCommandLineWDetour() {
  onStartupCall("GetCommandLineW");
  return originalGetCommandLineW();
}

static VOID WINAPI getStartupInfoADetour(LPSTARTUPINFOA info) {
  onStartupCall("GetStartupInfoA");
  originalGetStartupInfoA(info);
}

static VOID WINAPI getStartupInfoWDetour(LPSTARTUPINFOW info) {
  onStartupCall("GetStartupInfoW");
  originalGetStartupInfoW(info);
}

static DWORD WINAPI getVersionDetour() {
  onStartupCall("GetVersion");
  return originalGetVersion();
}

static BOOL WINAPI getVersionExADetour(LPOSVERSIONINFOA info) {
  onStartupCall("GetVersionExA");
  return originalGetVersionExA(info);
}

// An unpacking loader, such as Steam's DRM wrapper, loads the game's DLLs and resolves their functions itself, and checks
// for Steam before the game's C runtime starts.
static HMODULE WINAPI loadLibraryADetour(LPCSTR name) {
  onStartupCall("LoadLibraryA");
  return originalLoadLibraryA(name);
}

static HMODULE WINAPI loadLibraryWDetour(LPCWSTR name) {
  onStartupCall("LoadLibraryW");
  return originalLoadLibraryW(name);
}

static FARPROC WINAPI getProcAddressDetour(HMODULE module, LPCSTR name) {
  onStartupCall("GetProcAddress");
  return originalGetProcAddress(module, name);
}

// Created from DllMain, it runs once the loader lock is released.
static DWORD WINAPI watchStart(LPVOID parameter) {
  (void)parameter;
  if (WaitForSingleObject(startedEvent, lateStartSeconds * 1000) == WAIT_TIMEOUT) {
    fatalError("fixLoader was loaded after the game started and the game made none of the startup calls it waits for "
      "within %lu seconds, so no tool was loaded", lateStartSeconds);
  }
  return 0;
}

// Raw MinHook rather than the hook registry: the session log is not open yet. No tool can hook these nine.
template <typename Function>
static void hookKernel32(HMODULE kernel32, const char* exportName, Function detour, Function* original) {
  void* target = reinterpret_cast<void*>(GetProcAddress(kernel32, exportName));
  if (target == nullptr) {
    fatalError("kernel32.dll does not export %s", exportName);
  }
  MH_STATUS status = MH_CreateHook(target, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
  if (status == MH_OK) {
    status = MH_EnableHook(target);
  }
  if (status != MH_OK) {
    fatalError("MinHook failed to hook kernel32!%s for the late start: %s", exportName, MH_StatusToString(status));
  }
}

void installLateStart(void (*onStart)()) {
  if (startCallback != nullptr) {
    fatalError("installLateStart called twice");
  }
  startCallback = onStart;
  loadingThreadID = GetCurrentThreadId();
  startedEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (startedEvent == nullptr) {
    fatalError("CreateEvent failed (error %lu)", GetLastError());
  }
  HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
  if (kernel32 == nullptr) {
    fatalError("kernel32.dll is not loaded (error %lu)", GetLastError());
  }
  hookKernel32(kernel32, "GetCommandLineA", &getCommandLineADetour, &originalGetCommandLineA);
  hookKernel32(kernel32, "GetCommandLineW", &getCommandLineWDetour, &originalGetCommandLineW);
  hookKernel32(kernel32, "GetStartupInfoA", &getStartupInfoADetour, &originalGetStartupInfoA);
  hookKernel32(kernel32, "GetStartupInfoW", &getStartupInfoWDetour, &originalGetStartupInfoW);
  hookKernel32(kernel32, "GetVersion", &getVersionDetour, &originalGetVersion);
  hookKernel32(kernel32, "GetVersionExA", &getVersionExADetour, &originalGetVersionExA);
  hookKernel32(kernel32, "LoadLibraryA", &loadLibraryADetour, &originalLoadLibraryA);
  hookKernel32(kernel32, "LoadLibraryW", &loadLibraryWDetour, &originalLoadLibraryW);
  hookKernel32(kernel32, "GetProcAddress", &getProcAddressDetour, &originalGetProcAddress);
  HANDLE watcher = CreateThread(nullptr, 0, &watchStart, nullptr, 0, nullptr);
  if (watcher == nullptr) {
    fatalError("Could not start the late start watcher (error %lu)", GetLastError());
  }
  CloseHandle(watcher);
}

const char* lateStartTrigger() {
  return triggerName;
}
