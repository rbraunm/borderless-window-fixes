#include "freezeWatchdog.h"

#include "addressNames.h"
#include "callStack.h"
#include "fatalError.h"
#include "memoryDumps.h"
#include "sessionLog.h"

namespace {

// Standard dialog boxes (message boxes and the like); one of those not responding is not the game frozen.
const char dialogClassName[] = "#32770";
const DWORD pollMilliseconds = 1000;
// Unbiased interrupt time leaves out sleep and hibernation, so a computer waking up is not a freeze.
const ULONGLONG unbiasedTicksPerSecond = 10000000ULL;

Configuration settings;
DWORD gameMainThreadID = 0;

// The main thread's first visible top-level window that is not a dialog box.
BOOL CALLBACK findGameWindow(HWND window, LPARAM foundWindow) {
  char className[64];
  if (IsWindowVisible(window) && GetAncestor(window, GA_ROOT) == window && GetWindow(window, GW_OWNER) == nullptr &&
      GetClassNameA(window, className, sizeof(className)) > 0 && lstrcmpiA(className, dialogClassName) != 0) {
    *reinterpret_cast<HWND*>(foundWindow) = window;
    return FALSE;
  }
  return TRUE;
}

bool isGameNotResponding() {
  HWND gameWindow = nullptr;
  EnumThreadWindows(gameMainThreadID, &findGameWindow, reinterpret_cast<LPARAM>(&gameWindow));
  return gameWindow != nullptr && IsHungAppWindow(gameWindow) && !IsDebuggerPresent();
}

ULONGLONG unbiasedSeconds() {
  ULONGLONG unbiasedTime = 0;
  QueryUnbiasedInterruptTime(&unbiasedTime);
  return unbiasedTime / unbiasedTicksPerSecond;
}

// The main thread is paused only long enough to copy its registers and stack, and the copy is logged
// after it runs again: paused inside the log's own lock, it would otherwise hold up this thread too.
void writeMainThreadState(unsigned long freezeNumber) {
  HANDLE mainThread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, gameMainThreadID);
  if (mainThread == nullptr) {
    writeLogLine("WARN", "freezeMainThreadUnavailable number=%lu error=%lu", freezeNumber, GetLastError());
    return;
  }
  CONTEXT context = {};
  context.ContextFlags = CONTEXT_FULL;
  uintptr_t slots[stackScanSlotLimit];
  size_t slotCount = 0;
  bool isCaptured = false;
  if (SuspendThread(mainThread) != static_cast<DWORD>(-1)) {
    isCaptured = GetThreadContext(mainThread, &context) != FALSE;
    if (isCaptured) {
      slotCount = copyStackSlots(contextStackPointer(&context), slots, settings.stackScanSlots);
    }
    ResumeThread(mainThread);
  }
  const DWORD captureError = GetLastError();
  CloseHandle(mainThread);
  if (!isCaptured) {
    writeLogLine("WARN", "freezeMainThreadUnavailable number=%lu error=%lu", freezeNumber, captureError);
    return;
  }
  char location[MAX_PATH + 32];
  describeAddress(contextInstructionPointer(&context), location, sizeof(location));
  writeLogLine("INFO", "freezeMainThread number=%lu address=0x%016llX location=%s", freezeNumber,
    static_cast<unsigned long long>(contextInstructionPointer(&context)), location);
  writeRegisters("freeze", &context);
  writeFrameWalk("freeze", &context);
  writeStackCandidates("freeze", slots, slotCount);
}

DWORD WINAPI freezeWatchdogThread(LPVOID) {
  unsigned long freezeCount = 0;
  bool isNotResponding = false;
  bool isFreezeReported = false;
  ULONGLONG notRespondingSince = 0;
  for (;;) {
    Sleep(pollMilliseconds);
    if (!isGameNotResponding()) {
      if (isFreezeReported) {
        writeLogLine("INFO", "freezeEnded number=%lu notRespondingSeconds=%llu", freezeCount, unbiasedSeconds() - notRespondingSince);
      }
      isNotResponding = false;
      isFreezeReported = false;
      continue;
    }
    if (!isNotResponding) {
      isNotResponding = true;
      notRespondingSince = unbiasedSeconds();
    }
    if (isFreezeReported || unbiasedSeconds() - notRespondingSince < settings.freezeSeconds) {
      continue;
    }
    isFreezeReported = true;
    freezeCount++;
    writeLogLine("ERROR", "freezeDetected number=%lu notRespondingSeconds=%llu", freezeCount, unbiasedSeconds() - notRespondingSince);
    writeMainThreadState(freezeCount);
    if (settings.writeMemoryDumps) {
      writeMemoryDump(DumpKind::freeze, nullptr, freezeCount);
    }
  }
}

}

void installFreezeWatchdog(const Configuration& configuration, DWORD mainThreadID) {
  settings = configuration;
  gameMainThreadID = mainThreadID;
  HANDLE thread = CreateThread(nullptr, 0, &freezeWatchdogThread, nullptr, 0, nullptr);
  if (thread == nullptr) {
    fatalError("Could not start the freeze watchdog (error %lu)", GetLastError());
  }
  CloseHandle(thread);
  writeLogLine("INFO", "freezeWatchdogInstalled freezeSeconds=%lu", configuration.freezeSeconds);
}
