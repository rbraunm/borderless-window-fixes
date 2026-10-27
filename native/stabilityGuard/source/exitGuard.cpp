#include "exitGuard.h"

#include "addressNames.h"
#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"

#include <windows.h>
#include <intrin.h>

typedef void (WINAPI* PostQuitMessageFunction)(int exitCode);

// Posted from the low-level mouse hook to the watchdog's own queue, so the (slow) check runs outside the hook
// callback, which Windows gives only a few hundred milliseconds.
static const UINT clickReleasedMessage = WM_APP + 1;
// Shorter than the quickest press of F4 with Ctrl+Alt held.
static const DWORD forceExitKeysPollMilliseconds = 20;
// How long a busy game window gets to answer "which part of you is here?".
static const UINT hitTestTimeoutMilliseconds = 250;
// Standard dialog boxes (message boxes and the like) use this class; closing one
// is never a request to quit the game.
static const char dialogClassName[] = "#32770";

static unsigned long exitGraceMilliseconds = 0;
static bool isHotkeyEnabled = false;
static DWORD gameMainThreadID = 0;
static HANDLE exitRequestedEvent = nullptr;
static HANDLE watchdogReadyEvent = nullptr;
static volatile LONG watchdogStartupError = 0;
// The watchdog thread's GetLastError for a hook it could not install.
static volatile LONG watchdogSystemError = 0;
// Written once by the first request; read by the watchdog after the event says it is set.
static void* volatile requestTrigger = nullptr;
static PostQuitMessageFunction originalPostQuitMessage = nullptr;
static DWORD watchdogThreadID = 0;

typedef DPI_AWARENESS_CONTEXT (WINAPI* SetThreadDpiAwarenessContextFunction)(DPI_AWARENESS_CONTEXT context);
typedef BOOL (WINAPI* PhysicalToLogicalPointFunction)(HWND window, LPPOINT point);
static PhysicalToLogicalPointFunction physicalToLogicalPoint = nullptr;
typedef HWND (WINAPI* HungWindowFromGhostWindowFunction)(HWND ghostWindow);
static HungWindowFromGhostWindowFunction hungWindowFromGhostWindow = nullptr;

static const char* triggerName() {
  return static_cast<const char*>(requestTrigger);
}

static void requestExit(const char* trigger) {
  // The first request names the trigger; later ones only re-signal.
  InterlockedCompareExchangePointer(&requestTrigger, const_cast<char*>(trigger), nullptr);
  SetEvent(exitRequestedEvent);
}

[[noreturn]] static void forceExit(const char* trigger, DWORD waitedMilliseconds) {
  writeLogLine("WARN", "exitForced trigger=%s waitedMilliseconds=%lu", trigger, waitedMilliseconds);
  TerminateProcess(GetCurrentProcess(), 0);
  for (;;) {
    Sleep(INFINITE);
  }
}

static bool isGameTopLevelWindow(HWND window) {
  if (window == nullptr || GetAncestor(window, GA_ROOT) != window || GetWindow(window, GW_OWNER) != nullptr) {
    return false;
  }
  if (GetWindowThreadProcessId(window, nullptr) != gameMainThreadID) {
    return false;
  }
  char className[64];
  if (GetClassNameA(window, className, sizeof(className)) == 0) {
    return false;
  }
  return lstrcmpiA(className, dialogClassName) != 0;
}

// Sees every message sent to the main thread's windows, including the WM_CLOSE
// and SC_CLOSE a click on the X button produces.
static LRESULT CALLBACK callWindowProcedureHook(int code, WPARAM sentByThisThread, LPARAM messageAddress) {
  if (code == HC_ACTION) {
    const CWPSTRUCT* message = reinterpret_cast<const CWPSTRUCT*>(messageAddress);
    const bool isCloseRequest = message->message == WM_CLOSE ||
      (message->message == WM_SYSCOMMAND && (message->wParam & 0xFFF0) == SC_CLOSE);
    if (isCloseRequest && isGameTopLevelWindow(message->hwnd)) {
      requestExit("closeRequest");
    }
  }
  return CallNextHookEx(nullptr, code, sentByThisThread, messageAddress);
}

// The watchdog thread's mouse hook. It only notes where a left click was released;
// deciding whether that was the game's close button happens on the watchdog loop.
static LRESULT CALLBACK lowLevelMouseHook(int code, WPARAM message, LPARAM eventAddress) {
  if (code == HC_ACTION && message == WM_LBUTTONUP) {
    const MSLLHOOKSTRUCT* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(eventAddress);
    PostThreadMessageA(watchdogThreadID, clickReleasedMessage, static_cast<WPARAM>(event->pt.x),
      static_cast<LPARAM>(event->pt.y));
  }
  return CallNextHookEx(nullptr, code, message, eventAddress);
}

// A click released over one of the game's own windows is a close request when the
// window itself says the point is its close button. The game's message loop keeps
// answering sent messages like this one even while it ignores the click.
static void handleClickReleased(LONG physicalX, LONG physicalY) {
  POINT screenPoint = { physicalX, physicalY };
  HWND window = WindowFromPoint(screenPoint);
  if (window == nullptr) {
    return;
  }
  HWND topLevel = GetAncestor(window, GA_ROOT);
  if (!isGameTopLevelWindow(topLevel)) {
    return;
  }
  // The game is not DPI aware, so the window expects its own logical coordinates.
  POINT windowPoint = screenPoint;
  if (!physicalToLogicalPoint(topLevel, &windowPoint)) {
    writeLogLine("WARN", "closeClickUnchecked reason=pointConversionFailed error=%lu", GetLastError());
    return;
  }
  DWORD_PTR hitTest = HTNOWHERE;
  if (!SendMessageTimeoutA(topLevel, WM_NCHITTEST, 0, MAKELPARAM(windowPoint.x, windowPoint.y),
      SMTO_ABORTIFHUNG | SMTO_BLOCK, hitTestTimeoutMilliseconds, &hitTest)) {
    // Too frozen to say what was clicked. Windows' own not-responding handling
    // covers a window in this state; Ctrl+Alt+F4 always does.
    writeLogLine("WARN", "closeClickUnchecked reason=windowDidNotAnswer error=%lu", GetLastError());
    return;
  }
  if (hitTest == HTCLOSE) {
    requestExit("closeButton");
  }
}

// Every running copy of the game reads the keyboard itself: a hotkey belongs to one process only, and a low-level
// keyboard hook misses the keys when Windows calls it first while its own game is in front.
static bool areForceExitKeysDown() {
  return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
    (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
}

// The window in front is this game's own, or the stand-in Windows shows for it while it does not respond.
static bool isGameInFront() {
  HWND window = GetForegroundWindow();
  if (window == nullptr) {
    return false;
  }
  HWND hungWindow = hungWindowFromGhostWindow(window);
  DWORD processID = 0;
  GetWindowThreadProcessId(hungWindow != nullptr ? hungWindow : window, &processID);
  return processID == GetCurrentProcessId();
}

static void WINAPI postQuitMessageDetour(int exitCode) {
  char caller[MAX_PATH + 32];
  describeAddress(reinterpret_cast<uintptr_t>(_ReturnAddress()), caller, sizeof(caller));
  // A quit ends only the calling thread's message loop. Other threads post them for their own loops (a video
  // decoder playing an intro movie may), so only the main thread's means the game is quitting.
  const bool isGameQuitting = GetCurrentThreadId() == gameMainThreadID;
  writeLogLine("INFO", "quitPosted exitCode=%d thread=%s threadID=%lu caller=%s", exitCode, isGameQuitting ? "main" : "other",
    GetCurrentThreadId(), caller);
  if (isGameQuitting) {
    requestExit("postQuitMessage");
  }
  originalPostQuitMessage(exitCode);
}

static DWORD WINAPI watchdogThread(LPVOID) {
  MSG message;
  // Creates this thread's message queue before the hooks are tied to it.
  PeekMessageA(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  // Mouse hook positions are physical pixels; per-monitor awareness makes this
  // thread's WindowFromPoint take them as they are.
  HMODULE user32 = GetModuleHandleA("user32.dll");
  SetThreadDpiAwarenessContextFunction setThreadDpiAwarenessContext =
    reinterpret_cast<SetThreadDpiAwarenessContextFunction>(
      reinterpret_cast<void*>(GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
  physicalToLogicalPoint = reinterpret_cast<PhysicalToLogicalPointFunction>(
    reinterpret_cast<void*>(GetProcAddress(user32, "PhysicalToLogicalPointForPerMonitorDPI")));
  hungWindowFromGhostWindow = reinterpret_cast<HungWindowFromGhostWindowFunction>(
    reinterpret_cast<void*>(GetProcAddress(user32, "HungWindowFromGhostWindow")));
  if (setThreadDpiAwarenessContext == nullptr || physicalToLogicalPoint == nullptr || hungWindowFromGhostWindow == nullptr) {
    InterlockedExchange(&watchdogStartupError, -1);
    SetEvent(watchdogReadyEvent);
    return 1;
  }
  if (setThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE) == nullptr) {
    InterlockedExchange(&watchdogStartupError, -3);
    SetEvent(watchdogReadyEvent);
    return 1;
  }
  if (SetWindowsHookExA(WH_MOUSE_LL, &lowLevelMouseHook, GetModuleHandleA(nullptr), 0) == nullptr) {
    InterlockedExchange(&watchdogSystemError, static_cast<LONG>(GetLastError()));
    InterlockedExchange(&watchdogStartupError, -2);
    SetEvent(watchdogReadyEvent);
    return 1;
  }
  SetEvent(watchdogReadyEvent);

  DWORD armedTicks = 0;
  bool isArmed = false;
  // Only the moment the keys go down counts, so a game that comes to the front while they are still held (because
  // they just ended the game that was in front) keeps running.
  bool wereForceExitKeysDown = false;
  for (;;) {
    DWORD timeout = isHotkeyEnabled ? forceExitKeysPollMilliseconds : INFINITE;
    if (isArmed) {
      const DWORD elapsed = GetTickCount() - armedTicks;
      if (elapsed >= exitGraceMilliseconds) {
        forceExit(triggerName(), elapsed);
      }
      if (exitGraceMilliseconds - elapsed < timeout) {
        timeout = exitGraceMilliseconds - elapsed;
      }
    }
    const DWORD waitResult = MsgWaitForMultipleObjects(1, &exitRequestedEvent, FALSE, timeout, QS_ALLINPUT);
    if (waitResult == WAIT_OBJECT_0) {
      if (!isArmed) {
        isArmed = true;
        armedTicks = GetTickCount();
        writeLogLine("INFO", "exitRequested trigger=%s graceMilliseconds=%lu", triggerName(), exitGraceMilliseconds);
      }
    } else if (waitResult == WAIT_OBJECT_0 + 1) {
      while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == clickReleasedMessage) {
          handleClickReleased(static_cast<LONG>(message.wParam), static_cast<LONG>(message.lParam));
        }
      }
    } else if (waitResult != WAIT_TIMEOUT) {
      fatalError("Exit watchdog wait failed (error %lu)", GetLastError());
    }
    if (isHotkeyEnabled) {
      const bool areKeysDown = areForceExitKeysDown();
      if (areKeysDown && !wereForceExitKeysDown) {
        const bool isInFront = isGameInFront();
        writeLogLine("INFO", "forceExitKeysPressed gameInFront=%d", isInFront ? 1 : 0);
        if (isInFront) {
          forceExit("hotkey", isArmed ? GetTickCount() - armedTicks : 0);
        }
      }
      wereForceExitKeysDown = areKeysDown;
    }
  }
}

void installExitGuard(const Configuration& configuration, DWORD mainThreadID) {
  exitGraceMilliseconds = configuration.exitGraceSeconds * 1000;
  isHotkeyEnabled = configuration.forceExitHotkey;
  gameMainThreadID = mainThreadID;
  exitRequestedEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  watchdogReadyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  if (exitRequestedEvent == nullptr || watchdogReadyEvent == nullptr) {
    fatalError("Could not create the exit watchdog events (error %lu)", GetLastError());
  }

  installHook("user32!PostQuitMessage", resolveLoadedExport("user32.dll", "PostQuitMessage"),
    reinterpret_cast<void*>(&postQuitMessageDetour), reinterpret_cast<void**>(&originalPostQuitMessage));
  // Thread-specific, so it runs on the game's main thread for the main thread's windows.
  if (SetWindowsHookExA(WH_CALLWNDPROC, &callWindowProcedureHook, nullptr, mainThreadID) == nullptr) {
    fatalError("Could not watch the game window for close requests (error %lu)", GetLastError());
  }

  HANDLE thread = CreateThread(nullptr, 0, &watchdogThread, nullptr, 0, &watchdogThreadID);
  if (thread == nullptr) {
    fatalError("Could not start the exit watchdog (error %lu)", GetLastError());
  }
  CloseHandle(thread);
  WaitForSingleObject(watchdogReadyEvent, INFINITE);
  if (watchdogStartupError == -1) {
    fatalError("The exit guard needs Windows 10 (version 1607) or later: user32 lacks "
      "SetThreadDpiAwarenessContext, PhysicalToLogicalPointForPerMonitorDPI or HungWindowFromGhostWindow");
  }
  if (watchdogStartupError == -3) {
    fatalError("The exit guard could not make its thread per-monitor DPI aware");
  }
  if (watchdogStartupError == -2) {
    fatalError("Could not watch mouse clicks for the game's close button (error %ld)", watchdogSystemError);
  }
  writeLogLine("INFO", "exitGuardInstalled graceSeconds=%lu forceExitHotkey=%s", configuration.exitGraceSeconds,
    configuration.forceExitHotkey ? "Ctrl+Alt+F4" : "off");
}
