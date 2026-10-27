#include "pumpHooks.h"

#include "fatalError.h"
#include "hookRegistry.h"

#include <string>

namespace {

typedef BOOL(WINAPI* PeekMessageFunction)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL(WINAPI* GetMessageFunction)(LPMSG, HWND, UINT, UINT);

DWORD gameMainThreadID = 0;
// Held while the callback runs. A critical section, not a slim lock: the callback may run game code
// that pumps messages again on the same thread.
CRITICAL_SECTION callbackLock;
void (*pumpCallback)() = nullptr;
PeekMessageFunction originalPeekMessageA = nullptr;
PeekMessageFunction originalPeekMessageW = nullptr;
GetMessageFunction originalGetMessageA = nullptr;
GetMessageFunction originalGetMessageW = nullptr;

void runPumpCallback() {
  if (GetCurrentThreadId() != gameMainThreadID) {
    return;
  }
  EnterCriticalSection(&callbackLock);
  if (pumpCallback != nullptr) {
    pumpCallback();
  }
  LeaveCriticalSection(&callbackLock);
}

BOOL WINAPI peekMessageADetour(LPMSG message, HWND window, UINT first, UINT last, UINT removal) {
  runPumpCallback();
  return originalPeekMessageA(message, window, first, last, removal);
}

BOOL WINAPI peekMessageWDetour(LPMSG message, HWND window, UINT first, UINT last, UINT removal) {
  runPumpCallback();
  return originalPeekMessageW(message, window, first, last, removal);
}

BOOL WINAPI getMessageADetour(LPMSG message, HWND window, UINT first, UINT last) {
  runPumpCallback();
  return originalGetMessageA(message, window, first, last);
}

BOOL WINAPI getMessageWDetour(LPMSG message, HWND window, UINT first, UINT last) {
  runPumpCallback();
  return originalGetMessageW(message, window, first, last);
}

void hookUser32(const char* exportName, void* detour, void** original) {
  const std::string targetName = std::string("user32!") + exportName;
  installHook(targetName.c_str(), resolveLoadedExport("user32.dll", exportName), detour, original);
}

}

void installPumpHooks(DWORD mainThreadID) {
  gameMainThreadID = mainThreadID;
  InitializeCriticalSection(&callbackLock);
  hookUser32("PeekMessageA", reinterpret_cast<void*>(&peekMessageADetour), reinterpret_cast<void**>(&originalPeekMessageA));
  hookUser32("PeekMessageW", reinterpret_cast<void*>(&peekMessageWDetour), reinterpret_cast<void**>(&originalPeekMessageW));
  hookUser32("GetMessageA", reinterpret_cast<void*>(&getMessageADetour), reinterpret_cast<void**>(&originalGetMessageA));
  hookUser32("GetMessageW", reinterpret_cast<void*>(&getMessageWDetour), reinterpret_cast<void**>(&originalGetMessageW));
}

void setPumpCallback(void (*callback)()) {
  EnterCriticalSection(&callbackLock);
  const bool isTaken = callback != nullptr && pumpCallback != nullptr;
  if (!isTaken) {
    pumpCallback = callback;
  }
  LeaveCriticalSection(&callbackLock);
  if (isTaken) {
    fatalError("A tool set the pump callback while another tool holds it");
  }
}
