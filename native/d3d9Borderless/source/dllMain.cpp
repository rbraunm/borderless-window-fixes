#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"
#include "toolHost.h"

#include <windows.h>
#include <d3d9.h>
#include <winternl.h>

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
typedef UINT (STDMETHODCALLTYPE* GetAdapterModeCountFunction)(IDirect3D9*, UINT, D3DFORMAT);
typedef HRESULT (STDMETHODCALLTYPE* EnumAdapterModesFunction)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
typedef HRESULT (STDMETHODCALLTYPE* CreateDeviceFunction)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
  D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT (STDMETHODCALLTYPE* ResetFunction)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

static const char* const toolName = "d3d9Borderless";
static const char* const d3d9BorderlessVersion = "0.1.0";
static const ULONG dllLoadedReason = 1;
// Positions in the COM interfaces' method tables, from d3d9.h's declaration order.
static const size_t getAdapterModeCountIndex = 6;
static const size_t enumAdapterModesIndex = 7;
static const size_t createDeviceIndex = 16;
static const size_t resetIndex = 16;

static bool isLoaded = false;
static volatile LONG isD3d9Hooked = 0;
static volatile LONG isInterfaceHooked = 0;
static volatile LONG isDeviceHooked = 0;
static Direct3DCreate9Function originalDirect3DCreate9 = nullptr;
static GetAdapterModeCountFunction originalGetAdapterModeCount = nullptr;
static EnumAdapterModesFunction originalEnumAdapterModes = nullptr;
static CreateDeviceFunction originalCreateDevice = nullptr;
static ResetFunction originalReset = nullptr;

static void* methodOf(void* object, size_t index) {
  return (*reinterpret_cast<void***>(object))[index];
}

static MONITORINFOEXA monitorOf(HWND window) {
  MONITORINFOEXA info = {};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &info)) {
    fatalError("GetMonitorInfo failed (error %lu)", GetLastError());
  }
  return info;
}

static DEVMODEA desktopModeOf(const MONITORINFOEXA& monitor) {
  DEVMODEA mode = {};
  mode.dmSize = sizeof(mode);
  if (!EnumDisplaySettingsA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
    fatalError("EnumDisplaySettings failed for %s", monitor.szDevice);
  }
  return mode;
}

// The game's resolution list offers only the desktop mode of the adapter's monitor, in every format it asks about.
static UINT STDMETHODCALLTYPE getAdapterModeCountDetour(IDirect3D9* direct3d, UINT adapter, D3DFORMAT format) {
  const UINT realCount = originalGetAdapterModeCount(direct3d, adapter, format);
  return realCount == 0 ? 0 : 1;
}

static HRESULT STDMETHODCALLTYPE enumAdapterModesDetour(IDirect3D9* direct3d, UINT adapter, D3DFORMAT format, UINT modeIndex,
    D3DDISPLAYMODE* mode) {
  if (modeIndex != 0 || mode == nullptr) {
    return D3DERR_INVALIDCALL;
  }
  MONITORINFOEXA adapterMonitor = {};
  adapterMonitor.cbSize = sizeof(adapterMonitor);
  if (!GetMonitorInfoA(direct3d->GetAdapterMonitor(adapter), &adapterMonitor)) {
    fatalError("GetMonitorInfo failed for adapter %u (error %lu)", adapter, GetLastError());
  }
  const DEVMODEA desktop = desktopModeOf(adapterMonitor);
  mode->Width = desktop.dmPelsWidth;
  mode->Height = desktop.dmPelsHeight;
  mode->RefreshRate = desktop.dmDisplayFrequency;
  mode->Format = format;
  return D3D_OK;
}

// A borderless window over the whole monitor; borderlessFit, when listed, sizes one that matches the monitor exactly.
static void makeBorderless(HWND window, const char* reason) {
  const MONITORINFOEXA monitor = monitorOf(window);
  const RECT& bounds = monitor.rcMonitor;
  SetWindowLongPtrA(window, GWL_EXSTYLE, GetWindowLongPtrA(window, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_TOPMOST));
  SetWindowLongPtrA(window, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
  if (!SetWindowPos(window, HWND_NOTOPMOST, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
      SWP_FRAMECHANGED | SWP_SHOWWINDOW)) {
    fatalError("SetWindowPos failed for the game window (error %lu)", GetLastError());
  }
  writeLogLine("INFO", "windowBorderless reason=%s window=%p monitor=%s left=%ld top=%ld width=%ld height=%ld", reason,
    static_cast<void*>(window), monitor.szDevice, bounds.left, bounds.top, bounds.right - bounds.left,
    bounds.bottom - bounds.top);
}

static void forceWindowed(const char* reason, D3DPRESENT_PARAMETERS* parameters) {
  writeLogLine("INFO", "presentAsked reason=%s windowed=%d width=%u height=%u refreshRate=%u format=%d interval=0x%X window=%p",
    reason, parameters->Windowed, parameters->BackBufferWidth, parameters->BackBufferHeight,
    parameters->FullScreen_RefreshRateInHz, static_cast<int>(parameters->BackBufferFormat),
    parameters->PresentationInterval, static_cast<void*>(parameters->hDeviceWindow));
  parameters->Windowed = TRUE;
  parameters->FullScreen_RefreshRateInHz = 0;
}

static HRESULT STDMETHODCALLTYPE resetDetour(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* parameters) {
  forceWindowed("reset", parameters);
  const HRESULT result = originalReset(device, parameters);
  writeLogLine("INFO", "deviceReset result=0x%08lX", static_cast<unsigned long>(result));
  D3DDEVICE_CREATION_PARAMETERS creation = {};
  if (SUCCEEDED(result) && SUCCEEDED(device->GetCreationParameters(&creation))) {
    makeBorderless(parameters->hDeviceWindow != nullptr ? parameters->hDeviceWindow : creation.hFocusWindow, "reset");
  }
  return result;
}

static HRESULT STDMETHODCALLTYPE createDeviceDetour(IDirect3D9* direct3d, UINT adapter, D3DDEVTYPE deviceType,
    HWND focusWindow, DWORD behaviorFlags, D3DPRESENT_PARAMETERS* parameters, IDirect3DDevice9** device) {
  forceWindowed("create", parameters);
  const HRESULT result = originalCreateDevice(direct3d, adapter, deviceType, focusWindow, behaviorFlags, parameters, device);
  writeLogLine("INFO", "deviceCreated result=0x%08lX adapter=%u", static_cast<unsigned long>(result), adapter);
  if (FAILED(result)) {
    return result;
  }
  if (InterlockedExchange(&isDeviceHooked, 1) == 0) {
    installHook("IDirect3DDevice9::Reset", methodOf(*device, resetIndex), reinterpret_cast<void*>(&resetDetour),
      reinterpret_cast<void**>(&originalReset));
  }
  makeBorderless(parameters->hDeviceWindow != nullptr ? parameters->hDeviceWindow : focusWindow, "create");
  return result;
}

// Every IDirect3D9 shares one method table, so the first object's methods cover all of them.
static IDirect3D9* WINAPI direct3DCreate9Detour(UINT sdkVersion) {
  IDirect3D9* direct3d = originalDirect3DCreate9(sdkVersion);
  if (direct3d != nullptr && InterlockedExchange(&isInterfaceHooked, 1) == 0) {
    installHook("IDirect3D9::GetAdapterModeCount", methodOf(direct3d, getAdapterModeCountIndex),
      reinterpret_cast<void*>(&getAdapterModeCountDetour), reinterpret_cast<void**>(&originalGetAdapterModeCount));
    installHook("IDirect3D9::EnumAdapterModes", methodOf(direct3d, enumAdapterModesIndex),
      reinterpret_cast<void*>(&enumAdapterModesDetour), reinterpret_cast<void**>(&originalEnumAdapterModes));
    installHook("IDirect3D9::CreateDevice", methodOf(direct3d, createDeviceIndex), reinterpret_cast<void*>(&createDeviceDetour),
      reinterpret_cast<void**>(&originalCreateDevice));
  }
  return direct3d;
}

static void hookD3d9() {
  if (InterlockedExchange(&isD3d9Hooked, 1) != 0) {
    return;
  }
  installExportHook("d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(&direct3DCreate9Detour),
    reinterpret_cast<void**>(&originalDirect3DCreate9));
}

static VOID CALLBACK onDllNotification(ULONG reason, const DllNotificationData* data, PVOID context) {
  (void)context;
  if (reason == dllLoadedReason && data->baseDllName != nullptr &&
      _wcsnicmp(data->baseDllName->Buffer, L"d3d9.dll", data->baseDllName->Length / sizeof(wchar_t)) == 0 &&
      data->baseDllName->Length == wcslen(L"d3d9.dll") * sizeof(wchar_t)) {
    hookD3d9();
  }
}

extern "C" {

// Turns a Direct3D 9 game's exclusive fullscreen into a borderless window at the desktop's own mode: the device is
// created windowed, the game's window covers its monitor, and the game's resolution list holds only the desktop mode,
// so focus changes never switch the display mode or minimize the game. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", d3d9BorderlessVersion,
    windowFixesBuildCommit, GetCurrentThreadId());
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
  } else {
    writeLogLine("INFO", "waitingForD3d9");
  }
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
