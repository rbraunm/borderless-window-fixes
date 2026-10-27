#include "focusReport.h"

#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

typedef BOOL (WINAPI* ClipCursorFunction)(const RECT*);
typedef HWND (WINAPI* SetCaptureFunction)(HWND);
typedef BOOL (WINAPI* ReleaseCaptureFunction)();
typedef int (WINAPI* ShowCursorFunction)(BOOL);
typedef BOOL (WINAPI* SetCursorPosFunction)(int, int);
typedef BOOL (WINAPI* RegisterRawInputDevicesFunction)(PCRAWINPUTDEVICE, UINT, UINT);
typedef HHOOK (WINAPI* SetWindowsHookExWFunction)(int, HOOKPROC, HINSTANCE, DWORD);
typedef HHOOK (WINAPI* SetWindowsHookExAFunction)(int, HOOKPROC, HINSTANCE, DWORD);
typedef BOOL (WINAPI* ShowWindowFunction)(HWND, int);
typedef BOOL (WINAPI* SetForegroundWindowFunction)(HWND);
typedef BOOL (WINAPI* SwapBuffersFunction)(HDC);
// The system call every display mode change ends in, whoever asks; its arguments are not part of any public
// header and differ between Windows versions, so they are passed through unread. x64's caller removes them; on x86 the
// stub does (ret 0x10), so the count must match it exactly.
#if defined(_M_IX86)
typedef LONG (WINAPI* NtUserChangeDisplaySettingsFunction)(void*, void*, void*, void*);
static const unsigned char ntUserChangeDisplaySettingsReturn[] = { 0xC2, 0x10, 0x00 };
#else
typedef LONG (WINAPI* NtUserChangeDisplaySettingsFunction)(void*, void*, void*, void*, void*);
#endif

// SDL2's public signatures; SDL_bool is an int-sized enum.
struct SdlVersion {
  unsigned char major;
  unsigned char minor;
  unsigned char patch;
};
typedef void (*SdlGetVersionFunction)(SdlVersion*);
typedef const char* (*SdlGetHintFunction)(const char*);
typedef int (*SdlSetHintWithPriorityFunction)(const char*, const char*, int);
typedef void (*SdlSetWindowGrabFunction)(void*, int);
typedef int (*SdlSetRelativeMouseModeFunction)(int);
typedef void (*SdlWindowFunction)(void*);
typedef int (*SdlSetWindowFullscreenFunction)(void*, unsigned int);
typedef int (*SdlShowCursorFunction)(int);

const char* const sdlModuleName = "SDL2.dll";
const char* const reportedSdlHints[] = { "SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", "SDL_MOUSE_RELATIVE_MODE_WARP",
  "SDL_GRAB_KEYBOARD", "SDL_VIDEO_WINDOW_SHARE_PIXEL_FORMAT" };
const LONGLONG stallMilliseconds = 200;
const LONGLONG burstMilliseconds = 10000;
const LONGLONG summaryMilliseconds = 60000;
const int loggedSwapFailures = 5;

DWORD gameMainThreadID = 0;
LARGE_INTEGER counterFrequency = {};

ClipCursorFunction originalClipCursor = nullptr;
SetCaptureFunction originalSetCapture = nullptr;
ReleaseCaptureFunction originalReleaseCapture = nullptr;
ShowCursorFunction originalShowCursor = nullptr;
SetCursorPosFunction originalSetCursorPos = nullptr;
RegisterRawInputDevicesFunction originalRegisterRawInputDevices = nullptr;
SetWindowsHookExWFunction originalSetWindowsHookExW = nullptr;
SetWindowsHookExAFunction originalSetWindowsHookExA = nullptr;
ShowWindowFunction originalShowWindow = nullptr;
SetForegroundWindowFunction originalSetForegroundWindow = nullptr;
SwapBuffersFunction originalSwapBuffers = nullptr;
NtUserChangeDisplaySettingsFunction originalNtUserChangeDisplaySettings = nullptr;
SdlGetHintFunction sdlGetHint = nullptr;
SdlSetHintWithPriorityFunction originalSdlSetHintWithPriority = nullptr;
SdlSetWindowGrabFunction originalSdlSetWindowGrab = nullptr;
SdlSetRelativeMouseModeFunction originalSdlSetRelativeMouseMode = nullptr;
SdlWindowFunction originalSdlMinimizeWindow = nullptr;
SdlWindowFunction originalSdlRestoreWindow = nullptr;
SdlSetWindowFullscreenFunction originalSdlSetWindowFullscreen = nullptr;
SdlShowCursorFunction originalSdlShowCursor = nullptr;

// Frames per second are logged until this counter value: activation changes move it.
std::atomic<LONGLONG> burstUntil(0);
std::atomic<long> cursorWarps(0);
std::atomic<bool> hasLoggedCursorWarp(false);
// SOMA sets both every frame; only a change of value is logged.
std::atomic<int> loggedSdlGrab(-1);
std::atomic<int> loggedSdlRelativeMode(-1);

// Mouse confinement and visibility as last logged, so repeated identical calls stay out of the log.
CRITICAL_SECTION cursorStateLock;
bool hasLoggedClip = false;
bool wasClipped = false;
RECT loggedClip = {};
int loggedCursorShown = -1;

// Touched only by the thread that presents frames.
LONGLONG lastSwap = 0;
LONGLONG windowStart = 0;
long windowFrames = 0;
LONGLONG windowMaxGap = 0;
LONGLONG summaryStart = 0;
long summaryFrames = 0;
LONGLONG summaryMaxGap = 0;
long summaryStalls = 0;
long swapFailures = 0;
bool hasLoggedSdlHints = false;

LONGLONG counterNow() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

LONGLONG toMilliseconds(LONGLONG ticks) {
  return ticks * 1000 / counterFrequency.QuadPart;
}

bool isTopLevel(HWND window) {
  return window != nullptr && GetAncestor(window, GA_ROOT) == window;
}

bool isGameInForeground() {
  DWORD processID = 0;
  HWND foreground = GetForegroundWindow();
  return foreground != nullptr && GetWindowThreadProcessId(foreground, &processID) != 0 && processID == GetCurrentProcessId();
}

// module+0xOFFSET of the code that called the hooked function, to tell SDL's calls from the game's.
const char* describeCaller(void* returnAddress, char* buffer, size_t bufferSize) {
  HMODULE module = nullptr;
  char modulePath[MAX_PATH];
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      static_cast<LPCSTR>(returnAddress), &module) || GetModuleFileNameA(module, modulePath, sizeof(modulePath)) == 0) {
    snprintf(buffer, bufferSize, "%p", returnAddress);
    return buffer;
  }
  const char* name = strrchr(modulePath, '\\');
  snprintf(buffer, bufferSize, "%s+0x%llX", name != nullptr ? name + 1 : modulePath,
    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(returnAddress) - reinterpret_cast<uintptr_t>(module)));
  return buffer;
}

void logCursorState(const char* reason) {
  RECT clip = {};
  GetClipCursor(&clip);
  CURSORINFO cursor = {};
  cursor.cbSize = sizeof(cursor);
  GetCursorInfo(&cursor);
  writeLogLine("INFO", "cursorState reason=%s clipLeft=%ld clipTop=%ld clipWidth=%ld clipHeight=%ld cursorShown=%s cursorX=%ld "
    "cursorY=%ld capture=%p foreground=%s", reason, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top,
    (cursor.flags & CURSOR_SHOWING) != 0 ? "true" : "false", cursor.ptScreenPos.x, cursor.ptScreenPos.y,
    static_cast<void*>(GetCapture()), isGameInForeground() ? "true" : "false");
}

void startFrameBurst() {
  burstUntil.store(counterNow() + burstMilliseconds * counterFrequency.QuadPart / 1000);
}

const char* sizeTypeName(WPARAM type) {
  return type == SIZE_RESTORED ? "restored" : type == SIZE_MINIMIZED ? "minimized" : type == SIZE_MAXIMIZED ? "maximized"
    : type == SIZE_MAXSHOW ? "maxShow" : type == SIZE_MAXHIDE ? "maxHide" : "unknown";
}

void logWindowMessage(const char* path, HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (!isTopLevel(window)) {
    return;
  }
  void* const handle = static_cast<void*>(window);
  switch (message) {
    case WM_ACTIVATEAPP:
      writeLogLine("INFO", "message path=%s window=%p name=WM_ACTIVATEAPP active=%s otherThreadID=%lu", path, handle,
        wParam != FALSE ? "true" : "false", static_cast<unsigned long>(lParam));
      startFrameBurst();
      logCursorState(wParam != FALSE ? "activated" : "deactivated");
      break;
    case WM_ACTIVATE:
      writeLogLine("INFO", "message path=%s window=%p name=WM_ACTIVATE state=%u minimized=%s other=%p", path, handle,
        static_cast<unsigned>(LOWORD(wParam)), HIWORD(wParam) != 0 ? "true" : "false", reinterpret_cast<void*>(lParam));
      break;
    case WM_NCACTIVATE:
      writeLogLine("INFO", "message path=%s window=%p name=WM_NCACTIVATE active=%s", path, handle, wParam != FALSE ? "true" : "false");
      break;
    case WM_SETFOCUS:
      writeLogLine("INFO", "message path=%s window=%p name=WM_SETFOCUS from=%p", path, handle, reinterpret_cast<void*>(wParam));
      break;
    case WM_KILLFOCUS:
      writeLogLine("INFO", "message path=%s window=%p name=WM_KILLFOCUS to=%p", path, handle, reinterpret_cast<void*>(wParam));
      break;
    case WM_SIZE:
      writeLogLine("INFO", "message path=%s window=%p name=WM_SIZE type=%s width=%u height=%u", path, handle, sizeTypeName(wParam),
        static_cast<unsigned>(LOWORD(lParam)), static_cast<unsigned>(HIWORD(lParam)));
      break;
    case WM_SHOWWINDOW:
      writeLogLine("INFO", "message path=%s window=%p name=WM_SHOWWINDOW show=%s status=%ld", path, handle,
        wParam != FALSE ? "true" : "false", static_cast<long>(lParam));
      break;
    case WM_WINDOWPOSCHANGED: {
      const WINDOWPOS* position = reinterpret_cast<const WINDOWPOS*>(lParam);
      writeLogLine("INFO", "message path=%s window=%p name=WM_WINDOWPOSCHANGED x=%d y=%d width=%d height=%d flags=0x%04X "
        "insertAfter=%p", path, handle, position->x, position->y, position->cx, position->cy, position->flags,
        static_cast<void*>(position->hwndInsertAfter));
      break;
    }
    case WM_SYSCOMMAND:
      writeLogLine("INFO", "message path=%s window=%p name=WM_SYSCOMMAND command=0x%04X", path, handle,
        static_cast<unsigned>(wParam & 0xFFF0));
      break;
    case WM_CAPTURECHANGED:
      writeLogLine("INFO", "message path=%s window=%p name=WM_CAPTURECHANGED newCapture=%p", path, handle, reinterpret_cast<void*>(lParam));
      break;
    case WM_DISPLAYCHANGE:
      writeLogLine("INFO", "message path=%s window=%p name=WM_DISPLAYCHANGE bits=%u width=%u height=%u", path, handle,
        static_cast<unsigned>(wParam), static_cast<unsigned>(LOWORD(lParam)), static_cast<unsigned>(HIWORD(lParam)));
      break;
    case WM_CLOSE:
      writeLogLine("INFO", "message path=%s window=%p name=WM_CLOSE", path, handle);
      break;
    default:
      break;
  }
}

LRESULT CALLBACK sentMessageHook(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION) {
    const CWPSTRUCT* sent = reinterpret_cast<const CWPSTRUCT*>(lParam);
    logWindowMessage("sent", sent->hwnd, sent->message, sent->wParam, sent->lParam);
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

// Posted messages are seen each time the game peeks at them; only their removal from the queue counts.
LRESULT CALLBACK postedMessageHook(int code, WPARAM wParam, LPARAM lParam) {
  if (code == HC_ACTION && wParam == PM_REMOVE) {
    const MSG* posted = reinterpret_cast<const MSG*>(lParam);
    if (posted->message == WM_QUIT) {
      writeLogLine("INFO", "message path=posted name=WM_QUIT exitCode=%lld", static_cast<long long>(posted->wParam));
    } else {
      logWindowMessage("posted", posted->hwnd, posted->message, posted->wParam, posted->lParam);
    }
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL WINAPI clipCursorDetour(const RECT* clip) {
  void* const returnAddress = _ReturnAddress();
  const BOOL result = originalClipCursor(clip);
  EnterCriticalSection(&cursorStateLock);
  const bool isClipped = clip != nullptr;
  const bool isChanged = !hasLoggedClip || isClipped != wasClipped || (isClipped && memcmp(clip, &loggedClip, sizeof(RECT)) != 0);
  if (isChanged) {
    hasLoggedClip = true;
    wasClipped = isClipped;
    loggedClip = isClipped ? *clip : RECT{};
  }
  LeaveCriticalSection(&cursorStateLock);
  if (isChanged) {
    char caller[MAX_PATH + 32];
    if (isClipped) {
      writeLogLine("INFO", "clipCursor left=%ld top=%ld width=%ld height=%ld result=%d foreground=%s caller=%s", clip->left, clip->top,
        clip->right - clip->left, clip->bottom - clip->top, result, isGameInForeground() ? "true" : "false",
        describeCaller(returnAddress, caller, sizeof(caller)));
    } else {
      writeLogLine("INFO", "clipCursor released result=%d foreground=%s caller=%s", result, isGameInForeground() ? "true" : "false",
        describeCaller(returnAddress, caller, sizeof(caller)));
    }
  }
  return result;
}

HWND WINAPI setCaptureDetour(HWND window) {
  void* const returnAddress = _ReturnAddress();
  const HWND previous = originalSetCapture(window);
  char caller[MAX_PATH + 32];
  writeLogLine("INFO", "setCapture window=%p previous=%p caller=%s", static_cast<void*>(window), static_cast<void*>(previous),
    describeCaller(returnAddress, caller, sizeof(caller)));
  return previous;
}

BOOL WINAPI releaseCaptureDetour() {
  void* const returnAddress = _ReturnAddress();
  const BOOL result = originalReleaseCapture();
  char caller[MAX_PATH + 32];
  writeLogLine("INFO", "releaseCapture result=%d caller=%s", result, describeCaller(returnAddress, caller, sizeof(caller)));
  return result;
}

int WINAPI showCursorDetour(BOOL show) {
  void* const returnAddress = _ReturnAddress();
  const int displayCount = originalShowCursor(show);
  const int isShown = displayCount >= 0 ? 1 : 0;
  EnterCriticalSection(&cursorStateLock);
  const bool isChanged = isShown != loggedCursorShown;
  loggedCursorShown = isShown;
  LeaveCriticalSection(&cursorStateLock);
  if (isChanged) {
    char caller[MAX_PATH + 32];
    writeLogLine("INFO", "showCursor shown=%s displayCount=%d caller=%s", isShown != 0 ? "true" : "false", displayCount,
      describeCaller(returnAddress, caller, sizeof(caller)));
  }
  return displayCount;
}

// Relative mouse modes warp the cursor every frame, so warps are counted and only the first is logged.
BOOL WINAPI setCursorPosDetour(int x, int y) {
  void* const returnAddress = _ReturnAddress();
  const BOOL result = originalSetCursorPos(x, y);
  cursorWarps.fetch_add(1);
  if (!hasLoggedCursorWarp.exchange(true)) {
    char caller[MAX_PATH + 32];
    writeLogLine("INFO", "firstCursorWarp x=%d y=%d result=%d caller=%s", x, y, result, describeCaller(returnAddress, caller, sizeof(caller)));
  }
  return result;
}

BOOL WINAPI registerRawInputDevicesDetour(PCRAWINPUTDEVICE devices, UINT count, UINT size) {
  void* const returnAddress = _ReturnAddress();
  const BOOL result = originalRegisterRawInputDevices(devices, count, size);
  char caller[MAX_PATH + 32];
  describeCaller(returnAddress, caller, sizeof(caller));
  for (UINT index = 0; index < count; index++) {
    writeLogLine("INFO", "rawInputRegistered usagePage=0x%02X usage=0x%02X flags=0x%08lX target=%p result=%d caller=%s",
      devices[index].usUsagePage, devices[index].usUsage, devices[index].dwFlags, static_cast<void*>(devices[index].hwndTarget),
      result, caller);
  }
  return result;
}

void logWindowsHook(const char* function, int hookType, DWORD threadID, HHOOK hook, void* returnAddress) {
  char caller[MAX_PATH + 32];
  writeLogLine("INFO", "windowsHookSet function=%s type=%d threadID=%lu hook=%p caller=%s", function, hookType, threadID,
    static_cast<void*>(hook), describeCaller(returnAddress, caller, sizeof(caller)));
}

HHOOK WINAPI setWindowsHookExWDetour(int hookType, HOOKPROC procedure, HINSTANCE module, DWORD threadID) {
  void* const returnAddress = _ReturnAddress();
  const HHOOK hook = originalSetWindowsHookExW(hookType, procedure, module, threadID);
  logWindowsHook("SetWindowsHookExW", hookType, threadID, hook, returnAddress);
  return hook;
}

HHOOK WINAPI setWindowsHookExADetour(int hookType, HOOKPROC procedure, HINSTANCE module, DWORD threadID) {
  void* const returnAddress = _ReturnAddress();
  const HHOOK hook = originalSetWindowsHookExA(hookType, procedure, module, threadID);
  logWindowsHook("SetWindowsHookExA", hookType, threadID, hook, returnAddress);
  return hook;
}

BOOL WINAPI showWindowDetour(HWND window, int command) {
  void* const returnAddress = _ReturnAddress();
  const BOOL wasVisible = originalShowWindow(window, command);
  if (isTopLevel(window)) {
    char caller[MAX_PATH + 32];
    writeLogLine("INFO", "showWindow window=%p command=%d wasVisible=%d caller=%s", static_cast<void*>(window), command, wasVisible,
      describeCaller(returnAddress, caller, sizeof(caller)));
  }
  return wasVisible;
}

BOOL WINAPI setForegroundWindowDetour(HWND window) {
  void* const returnAddress = _ReturnAddress();
  const BOOL result = originalSetForegroundWindow(window);
  char caller[MAX_PATH + 32];
  writeLogLine("INFO", "setForegroundWindow window=%p result=%d caller=%s", static_cast<void*>(window), result,
    describeCaller(returnAddress, caller, sizeof(caller)));
  return result;
}

#if defined(_M_IX86)
// A system call stub is a few instructions ending in ret with the size of its arguments; one that does not end in the
// expected return takes a different number of them on this Windows.
static void requireStubReturn(const char* moduleName, const char* exportName, const unsigned char* expectedReturn,
    size_t returnSize) {
  const unsigned char* stub = static_cast<const unsigned char*>(resolveLoadedExport(moduleName, exportName));
  const size_t stubLimit = 32;
  for (size_t offset = 0; offset + returnSize <= stubLimit; offset++) {
    if (memcmp(stub + offset, expectedReturn, returnSize) == 0) {
      return;
    }
  }
  fatalError("%s!%s does not end in the expected return: this Windows passes it a different number of arguments",
    moduleName, exportName);
}
#endif

#if defined(_M_IX86)
LONG WINAPI ntUserChangeDisplaySettingsDetour(void* first, void* second, void* third, void* fourth) {
  void* const returnAddress = _ReturnAddress();
  const LONG result = originalNtUserChangeDisplaySettings(first, second, third, fourth);
#else
LONG WINAPI ntUserChangeDisplaySettingsDetour(void* first, void* second, void* third, void* fourth, void* fifth) {
  void* const returnAddress = _ReturnAddress();
  const LONG result = originalNtUserChangeDisplaySettings(first, second, third, fourth, fifth);
#endif
  char caller[MAX_PATH + 32];
  writeLogLine("INFO", "ntUserChangeDisplaySettings result=%ld threadID=%lu foreground=%s caller=%s", result, GetCurrentThreadId(),
    isGameInForeground() ? "true" : "false", describeCaller(returnAddress, caller, sizeof(caller)));
  return result;
}

void logSdlHints() {
  for (const char* name : reportedSdlHints) {
    const char* value = sdlGetHint(name);
    writeLogLine("INFO", "sdlHint name=%s value=%s", name, value != nullptr ? value : "(unset)");
  }
}

void countFrame(LONGLONG now, BOOL result) {
  if (result == FALSE) {
    swapFailures++;
    if (swapFailures <= loggedSwapFailures) {
      writeLogLine("WARN", "swapFailed error=%lu failures=%ld", GetLastError(), swapFailures);
    }
  }
  if (lastSwap == 0) {
    windowStart = now;
    summaryStart = now;
    writeLogLine("INFO", "firstFrame threadID=%lu foreground=%s", GetCurrentThreadId(), isGameInForeground() ? "true" : "false");
  } else {
    const LONGLONG gap = now - lastSwap;
    if (gap > windowMaxGap) {
      windowMaxGap = gap;
    }
    if (gap > summaryMaxGap) {
      summaryMaxGap = gap;
    }
    if (toMilliseconds(gap) >= stallMilliseconds) {
      summaryStalls++;
      writeLogLine("INFO", "frameStall milliseconds=%lld foreground=%s", toMilliseconds(gap), isGameInForeground() ? "true" : "false");
    }
  }
  lastSwap = now;
  windowFrames++;
  summaryFrames++;
  if (toMilliseconds(now - windowStart) >= 1000) {
    if (windowStart < burstUntil.load()) {
      writeLogLine("INFO", "framesPerSecond frames=%ld maxGapMilliseconds=%lld foreground=%s", windowFrames, toMilliseconds(windowMaxGap),
        isGameInForeground() ? "true" : "false");
    }
    windowStart = now;
    windowFrames = 0;
    windowMaxGap = 0;
  }
  if (toMilliseconds(now - summaryStart) >= summaryMilliseconds) {
    const LONGLONG elapsed = toMilliseconds(now - summaryStart);
    writeLogLine("INFO", "frameSummary seconds=%lld frames=%ld framesPerSecond=%lld maxGapMilliseconds=%lld stalls=%ld cursorWarps=%ld "
      "swapFailures=%ld foreground=%s", elapsed / 1000, summaryFrames, summaryFrames * 1000LL / elapsed, toMilliseconds(summaryMaxGap),
      summaryStalls, cursorWarps.exchange(0), swapFailures, isGameInForeground() ? "true" : "false");
    summaryStart = now;
    summaryFrames = 0;
    summaryMaxGap = 0;
    summaryStalls = 0;
  }
}

BOOL WINAPI swapBuffersDetour(HDC deviceContext) {
  const BOOL result = originalSwapBuffers(deviceContext);
  // The game sets its hints before it renders, so the first frame shows what SDL will act on.
  if (!hasLoggedSdlHints && sdlGetHint != nullptr) {
    hasLoggedSdlHints = true;
    logSdlHints();
  }
  countFrame(counterNow(), result);
  return result;
}

int sdlSetHintWithPriorityDetour(const char* name, const char* value, int priority) {
  const int result = originalSdlSetHintWithPriority(name, value, priority);
  writeLogLine("INFO", "sdlSetHint name=%s value=%s priority=%d result=%d", name != nullptr ? name : "(null)",
    value != nullptr ? value : "(null)", priority, result);
  return result;
}

void sdlSetWindowGrabDetour(void* window, int grabbed) {
  originalSdlSetWindowGrab(window, grabbed);
  if (loggedSdlGrab.exchange(grabbed) == grabbed) {
    return;
  }
  writeLogLine("INFO", "sdlSetWindowGrab window=%p grabbed=%d foreground=%s", window, grabbed, isGameInForeground() ? "true" : "false");
}

int sdlSetRelativeMouseModeDetour(int enabled) {
  const int result = originalSdlSetRelativeMouseMode(enabled);
  if (loggedSdlRelativeMode.exchange(enabled) == enabled) {
    return result;
  }
  writeLogLine("INFO", "sdlSetRelativeMouseMode enabled=%d result=%d foreground=%s", enabled, result, isGameInForeground() ? "true" : "false");
  return result;
}

void sdlMinimizeWindowDetour(void* window) {
  writeLogLine("INFO", "sdlMinimizeWindow window=%p foreground=%s", window, isGameInForeground() ? "true" : "false");
  originalSdlMinimizeWindow(window);
}

void sdlRestoreWindowDetour(void* window) {
  writeLogLine("INFO", "sdlRestoreWindow window=%p foreground=%s", window, isGameInForeground() ? "true" : "false");
  originalSdlRestoreWindow(window);
}

int sdlSetWindowFullscreenDetour(void* window, unsigned int flags) {
  const int result = originalSdlSetWindowFullscreen(window, flags);
  writeLogLine("INFO", "sdlSetWindowFullscreen window=%p flags=0x%08X result=%d", window, flags, result);
  return result;
}

// -1 only asks whether the cursor is shown.
int sdlShowCursorDetour(int toggle) {
  const int result = originalSdlShowCursor(toggle);
  if (toggle != -1) {
    writeLogLine("INFO", "sdlShowCursor toggle=%d result=%d", toggle, result);
  }
  return result;
}

template <typename Function>
void hookExport(const char* moduleName, const char* exportName, Function detour, Function* original) {
  installExportHook(moduleName, exportName, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}

void installSdlReporting() {
  HMODULE sdl = GetModuleHandleA(sdlModuleName);
  if (sdl == nullptr) {
    writeLogLine("INFO", "sdlAbsent module=%s", sdlModuleName);
    return;
  }
  SdlGetVersionFunction getVersion = reinterpret_cast<SdlGetVersionFunction>(resolveLoadedExport(sdlModuleName, "SDL_GetVersion"));
  SdlVersion version = {};
  getVersion(&version);
  writeLogLine("INFO", "sdlLoaded version=%u.%u.%u", version.major, version.minor, version.patch);
  sdlGetHint = reinterpret_cast<SdlGetHintFunction>(resolveLoadedExport(sdlModuleName, "SDL_GetHint"));
  // SDL_SetHint goes through SDL_SetHintWithPriority, so one hook sees both.
  hookExport(sdlModuleName, "SDL_SetHintWithPriority", &sdlSetHintWithPriorityDetour, &originalSdlSetHintWithPriority);
  hookExport(sdlModuleName, "SDL_SetWindowGrab", &sdlSetWindowGrabDetour, &originalSdlSetWindowGrab);
  hookExport(sdlModuleName, "SDL_SetRelativeMouseMode", &sdlSetRelativeMouseModeDetour, &originalSdlSetRelativeMouseMode);
  hookExport(sdlModuleName, "SDL_MinimizeWindow", &sdlMinimizeWindowDetour, &originalSdlMinimizeWindow);
  hookExport(sdlModuleName, "SDL_RestoreWindow", &sdlRestoreWindowDetour, &originalSdlRestoreWindow);
  hookExport(sdlModuleName, "SDL_SetWindowFullscreen", &sdlSetWindowFullscreenDetour, &originalSdlSetWindowFullscreen);
  hookExport(sdlModuleName, "SDL_ShowCursor", &sdlShowCursorDetour, &originalSdlShowCursor);
}

}

void installFocusReporting(DWORD mainThreadID) {
  gameMainThreadID = mainThreadID;
  QueryPerformanceFrequency(&counterFrequency);
  InitializeCriticalSection(&cursorStateLock);
  // Thread hooks on the main thread, installed before SetWindowsHookEx is hooked so they stay out of its log.
  if (SetWindowsHookExW(WH_CALLWNDPROC, &sentMessageHook, nullptr, gameMainThreadID) == nullptr) {
    fatalError("Could not hook the main thread's sent messages (error %lu)", GetLastError());
  }
  if (SetWindowsHookExW(WH_GETMESSAGE, &postedMessageHook, nullptr, gameMainThreadID) == nullptr) {
    fatalError("Could not hook the main thread's posted messages (error %lu)", GetLastError());
  }
  hookExport("user32.dll", "ClipCursor", &clipCursorDetour, &originalClipCursor);
  hookExport("user32.dll", "SetCapture", &setCaptureDetour, &originalSetCapture);
  hookExport("user32.dll", "ReleaseCapture", &releaseCaptureDetour, &originalReleaseCapture);
  hookExport("user32.dll", "ShowCursor", &showCursorDetour, &originalShowCursor);
  hookExport("user32.dll", "SetCursorPos", &setCursorPosDetour, &originalSetCursorPos);
  hookExport("user32.dll", "RegisterRawInputDevices", &registerRawInputDevicesDetour, &originalRegisterRawInputDevices);
  hookExport("user32.dll", "SetWindowsHookExW", &setWindowsHookExWDetour, &originalSetWindowsHookExW);
  hookExport("user32.dll", "SetWindowsHookExA", &setWindowsHookExADetour, &originalSetWindowsHookExA);
  hookExport("user32.dll", "ShowWindow", &showWindowDetour, &originalShowWindow);
  hookExport("user32.dll", "SetForegroundWindow", &setForegroundWindowDetour, &originalSetForegroundWindow);
  hookExport("gdi32.dll", "SwapBuffers", &swapBuffersDetour, &originalSwapBuffers);
#if defined(_M_IX86)
  requireStubReturn("win32u.dll", "NtUserChangeDisplaySettings", ntUserChangeDisplaySettingsReturn,
    sizeof(ntUserChangeDisplaySettingsReturn));
#endif
  hookExport("win32u.dll", "NtUserChangeDisplaySettings", &ntUserChangeDisplaySettingsDetour, &originalNtUserChangeDisplaySettings);
  installSdlReporting();
  logCursorState("load");
}
