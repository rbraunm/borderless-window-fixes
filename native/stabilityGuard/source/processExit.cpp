#include "processExit.h"

#include "addressNames.h"
#include "callStack.h"
#include "hookRegistry.h"
#include "sessionLog.h"

#include <intrin.h>

namespace {

typedef VOID(WINAPI* ExitProcessFunction)(UINT);
typedef BOOL(WINAPI* TerminateProcessFunction)(HANDLE, UINT);

Configuration settings;
DWORD gameMainThreadID = 0;
ExitProcessFunction originalExitProcess = nullptr;
TerminateProcessFunction originalTerminateProcess = nullptr;

// The stack is read from the hook's return address up, so it holds the callers and none of this code's
// own frames.
void writeProcessExit(const char* via, UINT exitCode, void** returnAddressSlot) {
  char caller[MAX_PATH + 32];
  describeAddress(reinterpret_cast<uintptr_t>(*returnAddressSlot), caller, sizeof(caller));
  writeLogLine("INFO", "processExiting via=%s exitCode=%u thread=%s threadID=%lu caller=%s", via, exitCode,
    GetCurrentThreadId() == gameMainThreadID ? "main" : "other", GetCurrentThreadId(), caller);
  uintptr_t slots[stackScanSlotLimit];
  writeStackCandidates("exit", slots,
    copyStackSlots(reinterpret_cast<uintptr_t>(returnAddressSlot), slots, settings.stackScanSlots));
}

VOID WINAPI exitProcessDetour(UINT exitCode) {
  writeProcessExit("ExitProcess", exitCode, static_cast<void**>(_AddressOfReturnAddress()));
  originalExitProcess(exitCode);
}

BOOL WINAPI terminateProcessDetour(HANDLE process, UINT exitCode) {
  if (GetProcessId(process) == GetCurrentProcessId()) {
    writeProcessExit("TerminateProcess", exitCode, static_cast<void**>(_AddressOfReturnAddress()));
  }
  return originalTerminateProcess(process, exitCode);
}

}

void installProcessExitLogging(const Configuration& configuration, DWORD mainThreadID) {
  settings = configuration;
  gameMainThreadID = mainThreadID;
  installHook("kernel32!ExitProcess", resolveLoadedExport("kernel32.dll", "ExitProcess"), reinterpret_cast<void*>(&exitProcessDetour),
    reinterpret_cast<void**>(&originalExitProcess));
  installHook("kernel32!TerminateProcess", resolveLoadedExport("kernel32.dll", "TerminateProcess"),
    reinterpret_cast<void*>(&terminateProcessDetour), reinterpret_cast<void**>(&originalTerminateProcess));
}
