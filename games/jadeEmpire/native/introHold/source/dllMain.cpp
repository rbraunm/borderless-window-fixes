#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"
#include "toolHost.h"

#include <windows.h>
#include <d3d9.h>
#include <winternl.h>

#include <cstring>
#include <string>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

struct DllNotificationData {
  ULONG flags;
  const UNICODE_STRING* fullDllName;
  const UNICODE_STRING* baseDllName;
  PVOID dllBase;
  ULONG sizeOfImage;
};

typedef VOID (CALLBACK* DllNotificationFunction)(ULONG reason, const DllNotificationData* data, PVOID context);
typedef NTSTATUS (NTAPI* LdrRegisterDllNotificationFunction)(ULONG, DllNotificationFunction, PVOID, PVOID*);
typedef IDirect3D9* (WINAPI* Direct3DCreate9Function)(UINT);
typedef HRESULT (STDMETHODCALLTYPE* CreateDeviceFunction)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
  IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE* PresentFunction)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef void* (WINAPI* BinkOpenFunction)(const char*, unsigned int);

static const char* const toolName = "introHold";
static const char* const introHoldVersion = "0.1.0";
static const char* const gameProgramName = "JadeEmpire.exe";
static const char* const settingsFileName = "JadeEmpire.ini";
static const ULONG dllLoadedReason = 1;
static const size_t createDeviceIndex = 16;
static const size_t presentIndex = 17;

static bool isLoaded = false;
static volatile LONG isHolding = 0;
static volatile LONG isD3d9Hooked = 0;
static volatile LONG isBinkHooked = 0;
static volatile LONG isInterfaceHooked = 0;
static volatile LONG isDeviceHooked = 0;
static unsigned long framesHeld = 0;
static Direct3DCreate9Function originalDirect3DCreate9 = nullptr;
static CreateDeviceFunction originalCreateDevice = nullptr;
static PresentFunction originalPresent = nullptr;
static BinkOpenFunction originalBinkOpen = nullptr;

static void* methodOf(void* object, size_t index) {
  return (*reinterpret_cast<void***>(object))[index];
}

static HRESULT STDMETHODCALLTYPE presentDetour(IDirect3DDevice9* device, const RECT* source, const RECT* destination,
    HWND window, const RGNDATA* dirtyRegion) {
  if (isHolding != 0) {
    framesHeld++;
    return D3D_OK;
  }
  return originalPresent(device, source, destination, window, dirtyRegion);
}

static void* WINAPI binkOpenDetour(const char* name, unsigned int flags) {
  if (InterlockedExchange(&isHolding, 0) != 0) {
    writeLogLine("INFO", "holdEnded reason=movieOpened movie=%s framesHeld=%lu", name != nullptr ? name : "(null)", framesHeld);
  }
  return originalBinkOpen(name, flags);
}

static HRESULT STDMETHODCALLTYPE createDeviceDetour(IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE deviceType,
    HWND focusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device) {
  const HRESULT result = originalCreateDevice(direct3d, adapter, deviceType, focusWindow, behaviorFlags, parameters, device);
  if (SUCCEEDED(result) && InterlockedExchange(&isDeviceHooked, 1) == 0) {
    installHook("IDirect3DDevice9::Present", methodOf(*device, presentIndex), reinterpret_cast<void*>(&presentDetour),
      reinterpret_cast<void**>(&originalPresent));
  }
  return result;
}

static IDirect3D9* WINAPI direct3DCreate9Detour(UINT sdkVersion) {
  IDirect3D9* direct3d = originalDirect3DCreate9(sdkVersion);
  if (direct3d != nullptr && InterlockedExchange(&isInterfaceHooked, 1) == 0) {
    installHook("IDirect3D9::CreateDevice", methodOf(direct3d, createDeviceIndex), reinterpret_cast<void*>(&createDeviceDetour),
      reinterpret_cast<void**>(&originalCreateDevice));
  }
  return direct3d;
}

static bool isNamed(const DllNotificationData* data, const wchar_t* name) {
  return data->baseDllName != nullptr && data->baseDllName->Length == wcslen(name) * sizeof(wchar_t) &&
    _wcsnicmp(data->baseDllName->Buffer, name, wcslen(name)) == 0;
}

static void hookD3d9() {
  if (InterlockedExchange(&isD3d9Hooked, 1) == 0) {
    installExportHook("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&direct3DCreate9Detour),
      reinterpret_cast<void**>(&originalDirect3DCreate9));
  }
}

static void hookBink() {
  if (InterlockedExchange(&isBinkHooked, 1) == 0) {
    installExportHook("binkw32.dll", "_BinkOpen@8", reinterpret_cast<void*>(&binkOpenDetour),
      reinterpret_cast<void**>(&originalBinkOpen));
  }
}

static VOID CALLBACK onDllNotification(ULONG reason, const DllNotificationData* data, PVOID context) {
  (void)context;
  if (reason != dllLoadedReason) {
    return;
  }
  if (isNamed(data, L"d3d9.dll")) {
    hookD3d9();
  } else if (isNamed(data, L"binkw32.dll")) {
    hookBink();
  }
}

extern "C" {

// The game draws a frame or two of its loading art before its intro movies start; exclusive fullscreen's mode switch
// used to blank the monitor over them, a borderless window shows them. This presents nothing until the first movie
// opens. With NoMovies or NoLogos set in JadeEmpire.ini there is no intro movie to wait for, so it holds nothing. In the
// configuration program it does nothing. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", introHoldVersion, windowFixesBuildCommit,
    GetCurrentThreadId());
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
  const std::string settingsPath = std::string(executablePath) + "\\" + settingsFileName;
  const UINT noMovies = GetPrivateProfileIntA("Render", "NoMovies", 0, settingsPath.c_str());
  const UINT noLogos = GetPrivateProfileIntA("Render", "NoLogos", 0, settingsPath.c_str());
  if (noMovies != 0 || noLogos != 0) {
    writeLogLine("INFO", "holdSkipped reason=noIntroMovie noMovies=%u noLogos=%u", noMovies, noLogos);
    writeLogLine("INFO", "loadComplete");
    return 0;
  }
  isHolding = 1;
  initializeHookRegistry();
  HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  LdrRegisterDllNotificationFunction ldrRegisterDllNotification = ntdll == nullptr ? nullptr :
    reinterpret_cast<LdrRegisterDllNotificationFunction>(GetProcAddress(ntdll, "LdrRegisterDllNotification"));
  if (ldrRegisterDllNotification == nullptr) {
    fatalError("ntdll.dll does not export LdrRegisterDllNotification");
  }
  PVOID cookie = nullptr;
  const NTSTATUS status = ldrRegisterDllNotification(0, &onDllNotification, nullptr, &cookie);
  if (status < 0) {
    fatalError("LdrRegisterDllNotification failed (status 0x%08lX)", static_cast<unsigned long>(status));
  }
  if (GetModuleHandleA("d3d9.dll") != nullptr) {
    hookD3d9();
  }
  if (GetModuleHandleA("binkw32.dll") != nullptr) {
    hookBink();
  }
  writeLogLine("INFO", "holding until=firstMovie");
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
