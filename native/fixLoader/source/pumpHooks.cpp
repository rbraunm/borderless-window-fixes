#include "pumpHooks.h"

#include "fatalError.h"
#include "hookRegistry.h"

namespace {

typedef BOOL(WINAPI* PeekMessageFunction)(LPMSG, HWND, UINT, UINT, UINT);
typedef BOOL(WINAPI* GetMessageFunction)(LPMSG, HWND, UINT, UINT);

DWORD gameMainThreadID = 0;
const size_t pumpCallbackCapacity = 16;
// Held while the callbacks run. A critical section, not a slim lock: a callback may run game code
// that pumps messages again on the same thread.
CRITICAL_SECTION callbackLock;
void (*pumpCallbacks[pumpCallbackCapacity])() = {};
size_t pumpCallbackCount = 0;
PeekMessageFunction originalPeekMessageA = nullptr;
PeekMessageFunction originalPeekMessageW = nullptr;
GetMessageFunction originalGetMessageA = nullptr;
GetMessageFunction originalGetMessageW = nullptr;

void runPumpCallback() {
  if (GetCurrentThreadId() != gameMainThreadID) {
    return;
  }
  EnterCriticalSection(&callbackLock);
  for (size_t index = 0; index < pumpCallbackCount; index++) {
    pumpCallbacks[index]();
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

}

void installPumpHooks(DWORD mainThreadID) {
  gameMainThreadID = mainThreadID;
  InitializeCriticalSection(&callbackLock);
  installExportHook("user32.dll", "PeekMessageA", reinterpret_cast<void*>(&peekMessageADetour), reinterpret_cast<void**>(&originalPeekMessageA));
  installExportHook("user32.dll", "PeekMessageW", reinterpret_cast<void*>(&peekMessageWDetour), reinterpret_cast<void**>(&originalPeekMessageW));
  installExportHook("user32.dll", "GetMessageA", reinterpret_cast<void*>(&getMessageADetour), reinterpret_cast<void**>(&originalGetMessageA));
  installExportHook("user32.dll", "GetMessageW", reinterpret_cast<void*>(&getMessageWDetour), reinterpret_cast<void**>(&originalGetMessageW));
}

void addPumpCallback(void (*callback)()) {
  EnterCriticalSection(&callbackLock);
  bool isAdded = false;
  for (size_t index = 0; index < pumpCallbackCount; index++) {
    isAdded = isAdded || pumpCallbacks[index] == callback;
  }
  const bool isFull = pumpCallbackCount == pumpCallbackCapacity;
  if (!isAdded && !isFull && callback != nullptr) {
    pumpCallbacks[pumpCallbackCount++] = callback;
  }
  LeaveCriticalSection(&callbackLock);
  if (callback == nullptr || isAdded || isFull) {
    fatalError(callback == nullptr ? "A tool added a null pump callback" : isAdded ?
      "A tool added a pump callback it had already added" : "More than 16 pump callbacks were added");
  }
}

void removePumpCallback(void (*callback)()) {
  EnterCriticalSection(&callbackLock);
  bool isFound = false;
  for (size_t index = 0; index < pumpCallbackCount; index++) {
    if (isFound) {
      pumpCallbacks[index - 1] = pumpCallbacks[index];
    } else {
      isFound = pumpCallbacks[index] == callback;
    }
  }
  if (isFound) {
    pumpCallbackCount--;
  }
  LeaveCriticalSection(&callbackLock);
  if (!isFound) {
    fatalError("A tool removed a pump callback it had not added");
  }
}
