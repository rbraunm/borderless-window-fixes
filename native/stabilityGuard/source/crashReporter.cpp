#include "crashReporter.h"

#include "addressNames.h"
#include "callStack.h"
#include "fatalError.h"
#include "hookRegistry.h"
#include "memoryDumps.h"
#include "sessionLog.h"

#include <windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>

struct ExceptionKind {
  DWORD code;
  const char* name;
};

// Exceptions that mean something went wrong. Everything else (C++ throws,
// OutputDebugString, thread naming, and the like) is normal traffic and ignored.
static const ExceptionKind reportedKinds[] = {
  { EXCEPTION_ACCESS_VIOLATION, "accessViolation" },
  { EXCEPTION_IN_PAGE_ERROR, "inPageError" },
  { EXCEPTION_ILLEGAL_INSTRUCTION, "illegalInstruction" },
  { EXCEPTION_PRIV_INSTRUCTION, "privilegedInstruction" },
  { EXCEPTION_INT_DIVIDE_BY_ZERO, "integerDivideByZero" },
  { EXCEPTION_INT_OVERFLOW, "integerOverflow" },
  { EXCEPTION_FLT_DIVIDE_BY_ZERO, "floatDivideByZero" },
  { EXCEPTION_FLT_INVALID_OPERATION, "floatInvalidOperation" },
  { EXCEPTION_FLT_OVERFLOW, "floatOverflow" },
  { EXCEPTION_FLT_STACK_CHECK, "floatStackCheck" },
  { EXCEPTION_ARRAY_BOUNDS_EXCEEDED, "arrayBoundsExceeded" },
  { EXCEPTION_DATATYPE_MISALIGNMENT, "datatypeMisalignment" },
  { EXCEPTION_STACK_OVERFLOW, "stackOverflow" },
  { EXCEPTION_BREAKPOINT, "breakpoint" },
  { static_cast<DWORD>(0xC0000374), "heapCorruption" },
  { static_cast<DWORD>(0xC0000409), "stackBufferOverrun" },
};

typedef LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* SetUnhandledExceptionFilterFunction)(LPTOP_LEVEL_EXCEPTION_FILTER filter);


static Configuration settings;
static DWORD gameMainThreadID = 0;
static CRITICAL_SECTION reportLock;
static uintptr_t reportedAddresses[1024];
static unsigned long fullReportCount = 0;
static unsigned long repeatCount = 0;
static unsigned long suppressedCount = 0;
static unsigned long probeFaultCount = 0;
static volatile LONG fatalCount = 0;
static volatile LONG isFatalDumpTaken = 0;
static bool isInstalled = false;
// The filter the game (or a library) last installed. The reporter's own filter stays in Windows' slot
// and hands every unhandled exception on to this one.
static void* volatile gameExceptionFilter = nullptr;
static SetUnhandledExceptionFilterFunction originalSetUnhandledExceptionFilter = nullptr;
// A fault inside the reporter itself must not recurse into another report.
static thread_local bool isReporting = false;
// The exception this thread last reported at first chance, so the same exception turning fatal is not
// described twice.
static thread_local DWORD lastReportedCode = 0;
static thread_local uintptr_t lastReportedAddress = 0;

static const char* exceptionName(DWORD code) {
  for (const ExceptionKind& kind : reportedKinds) {
    if (kind.code == code) {
      return kind.name;
    }
  }
  return nullptr;
}

// IsBadReadPtr and its relatives probe memory by faulting inside kernel32 or
// KernelBase and catching it there. Those faults are expected, not crashes.
static bool isSystemProbeFault(DWORD code, uintptr_t address) {
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR) {
    return false;
  }
  char location[MAX_PATH + 32];
  describeAddress(address, location, sizeof(location));
  return _strnicmp(location, "kernel32.dll+", 13) == 0 || _strnicmp(location, "kernelbase.dll+", 15) == 0;
}

static void writeFaultDetail(const EXCEPTION_RECORD* record, char* buffer, size_t bufferSize) {
  if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
      record->NumberParameters >= 2) {
    const ULONG_PTR accessKind = record->ExceptionInformation[0];
    const char* access = accessKind == 0 ? "read" : accessKind == 1 ? "write" : accessKind == 8 ? "execute" : "unknown";
    snprintf(buffer, bufferSize, " access=%s target=0x%016llX", access,
      static_cast<unsigned long long>(record->ExceptionInformation[1]));
  } else {
    buffer[0] = '\0';
  }
}

static void writeThreadDetail(const CONTEXT* context) {
  writeFrameWalk("crash", context);
  uintptr_t slots[stackScanSlotLimit];
  writeStackCandidates("crash", slots, copyStackSlots(contextStackPointer(context), slots, settings.stackScanSlots));
  writeModuleList("crash");
}

// Once per process: the first fatal exception is the one the game dies of.
static void writeFatalDump(EXCEPTION_POINTERS* exception) {
  if (!settings.writeMemoryDumps || InterlockedExchange(&isFatalDumpTaken, 1) != 0) {
    return;
  }
  writeMemoryDump(DumpKind::fatal, exception, 1);
}

static void writeFullReport(EXCEPTION_POINTERS* exception, const char* name, bool isStackOverflow) {
  const EXCEPTION_RECORD* record = exception->ExceptionRecord;
  const uintptr_t address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
  fullReportCount++;
  char location[MAX_PATH + 32];
  describeAddress(address, location, sizeof(location));
  char faultDetail[96];
  writeFaultDetail(record, faultDetail, sizeof(faultDetail));
  writeLogLine("ERROR", "crashReport number=%lu code=0x%08lX name=%s thread=%s threadID=%lu address=0x%016llX location=%s%s",
    fullReportCount, record->ExceptionCode, name, GetCurrentThreadId() == gameMainThreadID ? "main" : "other",
    GetCurrentThreadId(), static_cast<unsigned long long>(address), location, faultDetail);
  writeRegisters("crash", exception->ContextRecord);
  // An overflowed stack has almost no room left, so only the essentials are written here; the dump,
  // written by another thread, holds the rest. A thread that overflowed its stack does not recover.
  if (isStackOverflow) {
    writeFatalDump(exception);
    writeLogLine("INFO", "crashReportEnd number=%lu detail=registersOnly", fullReportCount);
    return;
  }
  writeThreadDetail(exception->ContextRecord);
  if (settings.writeMemoryDumps) {
    writeMemoryDump(DumpKind::crash, exception, fullReportCount);
  }
  writeLogLine("INFO", "crashReportEnd number=%lu", fullReportCount);
}

static LONG CALLBACK firstChanceHandler(EXCEPTION_POINTERS* exception) {
  const EXCEPTION_RECORD* record = exception->ExceptionRecord;
  const char* name = exceptionName(record->ExceptionCode);
  if (name == nullptr || isReporting || isDumpThread()) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  isReporting = true;
  const uintptr_t address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);

  EnterCriticalSection(&reportLock);
  if (isSystemProbeFault(record->ExceptionCode, address)) {
    probeFaultCount++;
  } else {
    bool isRepeat = false;
    for (unsigned long reportIndex = 0; reportIndex < fullReportCount; reportIndex++) {
      if (reportedAddresses[reportIndex] == address) {
        isRepeat = true;
        break;
      }
    }
    if (isRepeat) {
      repeatCount++;
      lastReportedCode = record->ExceptionCode;
      lastReportedAddress = address;
      char location[MAX_PATH + 32];
      describeAddress(address, location, sizeof(location));
      writeLogLine("WARN", "crashRepeat code=0x%08lX name=%s address=0x%016llX location=%s",
        record->ExceptionCode, name, static_cast<unsigned long long>(address), location);
    } else if (fullReportCount < settings.maxFullReports) {
      reportedAddresses[fullReportCount] = address;
      lastReportedCode = record->ExceptionCode;
      lastReportedAddress = address;
      writeFullReport(exception, name, record->ExceptionCode == EXCEPTION_STACK_OVERFLOW);
    } else {
      if (suppressedCount == 0) {
        writeLogLine("WARN", "crashReportLimitReached maxFullReports=%lu (later crashes are only counted)",
          settings.maxFullReports);
      }
      suppressedCount++;
    }
  }
  LeaveCriticalSection(&reportLock);

  isReporting = false;
  return EXCEPTION_CONTINUE_SEARCH;
}

// Windows calls this when nothing else handled an exception: the game is going down. It runs before the
// filter the game installed, which still gets the exception afterwards.
static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exception) {
  const EXCEPTION_RECORD* record = exception->ExceptionRecord;
  if (!isReporting && !isDumpThread()) {
    isReporting = true;
    const uintptr_t address = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
    InterlockedIncrement(&fatalCount);
    const char* name = exceptionName(record->ExceptionCode);
    char location[MAX_PATH + 32];
    describeAddress(address, location, sizeof(location));
    writeLogLine("ERROR", "crashFatal code=0x%08lX name=%s thread=%s threadID=%lu address=0x%016llX location=%s",
      record->ExceptionCode, name != nullptr ? name : "other", GetCurrentThreadId() == gameMainThreadID ? "main" : "other",
      GetCurrentThreadId(), static_cast<unsigned long long>(address), location);
    const bool isAlreadyDescribed = record->ExceptionCode == lastReportedCode && address == lastReportedAddress;
    if (!isAlreadyDescribed && record->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
      writeRegisters("crash", exception->ContextRecord);
      writeThreadDetail(exception->ContextRecord);
    }
    writeFatalDump(exception);
    isReporting = false;
  }
  LPTOP_LEVEL_EXCEPTION_FILTER gameFilter = reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(gameExceptionFilter);
  return gameFilter != nullptr ? gameFilter(exception) : EXCEPTION_CONTINUE_SEARCH;
}

static LPTOP_LEVEL_EXCEPTION_FILTER WINAPI setUnhandledExceptionFilterDetour(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
  char caller[MAX_PATH + 32];
  describeAddress(reinterpret_cast<uintptr_t>(_ReturnAddress()), caller, sizeof(caller));
  writeLogLine("INFO", "exceptionFilterSet filter=%p caller=%s", reinterpret_cast<void*>(filter), caller);
  return reinterpret_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(InterlockedExchangePointer(&gameExceptionFilter, reinterpret_cast<void*>(filter)));
}

void installCrashReporter(const Configuration& configuration, DWORD mainThreadID) {
  if (isInstalled) {
    fatalError("installCrashReporter called twice");
  }
  if (configuration.maxFullReports > sizeof(reportedAddresses) / sizeof(reportedAddresses[0])) {
    fatalError("stabilityGuard.cfg: maxFullReports must be at most %u",
      static_cast<unsigned>(sizeof(reportedAddresses) / sizeof(reportedAddresses[0])));
  }
  settings = configuration;
  gameMainThreadID = mainThreadID;
  InitializeCriticalSection(&reportLock);
  // First in line, so the report is written before any game handler runs.
  if (AddVectoredExceptionHandler(1, &firstChanceHandler) == nullptr) {
    fatalError("Could not install the exception handler (error %lu)", GetLastError());
  }
  installHook("kernel32!SetUnhandledExceptionFilter", resolveLoadedExport("kernel32.dll", "SetUnhandledExceptionFilter"),
    reinterpret_cast<void*>(&setUnhandledExceptionFilterDetour), reinterpret_cast<void**>(&originalSetUnhandledExceptionFilter));
  gameExceptionFilter = reinterpret_cast<void*>(originalSetUnhandledExceptionFilter(&unhandledExceptionFilter));
  isInstalled = true;
  writeLogLine("INFO", "crashReporterInstalled previousFilter=%p", gameExceptionFilter);
}

void writeCrashTotals() {
  if (!isInstalled) {
    return;
  }
  writeLogLine("INFO", "sessionTotals fullReports=%lu repeats=%lu suppressed=%lu systemProbeFaultsIgnored=%lu fatal=%ld",
    fullReportCount, repeatCount, suppressedCount, probeFaultCount, fatalCount);
}
