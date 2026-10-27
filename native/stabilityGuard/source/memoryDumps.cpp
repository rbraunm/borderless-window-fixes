#include "memoryDumps.h"

#include "fatalError.h"
#include "sessionLog.h"

#include <dbghelp.h>

#include <cstdio>
#include <cstring>

namespace {

typedef BOOL(WINAPI* MiniDumpWriteDumpFunction)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Room for a dump of all of the game's memory to reach the disk. A dump thread still busy after this is
// stuck, most likely on a lock the crashed thread holds, and the crashed thread carries on without it.
const DWORD dumpWaitMilliseconds = 120000;
// Paths under the game folder, which can be longer than MAX_PATH with Windows long paths. Fixed buffers,
// so writing a dump does not depend on the heap.
const size_t dumpPathBytes = 512;
const unsigned long long bytesPerMegabyte = 1024ULL * 1024ULL;
const unsigned long long bytesPerGigabyte = 1024ULL * bytesPerMegabyte;

const MINIDUMP_TYPE stackDumpType = static_cast<MINIDUMP_TYPE>(
  MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
const MINIDUMP_TYPE fullDumpType = static_cast<MINIDUMP_TYPE>(
  MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
  MiniDumpWithUnloadedModules | MiniDumpIgnoreInaccessibleMemory);

struct DumpRequest {
  DumpKind kind;
  EXCEPTION_POINTERS* exception;
  DWORD threadID;
  unsigned long number;
  bool isWritten;
};

MiniDumpWriteDumpFunction miniDumpWriteDump = nullptr;
char dumpDirectory[dumpPathBytes];
unsigned long long maxDumpBytes = 0;
CRITICAL_SECTION requestLock;
HANDLE requestEvent = nullptr;
HANDLE doneEvent = nullptr;
DWORD dumpThreadID = 0;
// Both guarded by requestLock.
DumpRequest request;
bool isDumpThreadStuck = false;

const char* kindName(DumpKind kind) {
  return kind == DumpKind::crash ? "crash" : kind == DumpKind::fatal ? "fatal" : "freeze";
}

unsigned long long fileBytes(const WIN32_FIND_DATAA& file) {
  return (static_cast<unsigned long long>(file.nFileSizeHigh) << 32) | file.nFileSizeLow;
}

unsigned long long fileTimeValue(const FILETIME& fileTime) {
  return (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
}

// Removes the oldest dumps, never keptName, until the rest fit under maxDumpBytes. False when the folder
// could not be read or a dump could not be removed.
bool removeDumpsOverLimit(const char* keptName) {
  if (maxDumpBytes == 0) {
    return true;
  }
  char searchPattern[dumpPathBytes];
  snprintf(searchPattern, sizeof(searchPattern), "%s\\stabilityGuard_*.dmp", dumpDirectory);
  for (;;) {
    unsigned long long totalBytes = 0;
    unsigned long long oldestTime = ~0ULL;
    unsigned long long oldestBytes = 0;
    char oldestName[MAX_PATH] = "";
    WIN32_FIND_DATAA foundFile;
    HANDLE searchHandle = FindFirstFileA(searchPattern, &foundFile);
    if (searchHandle == INVALID_HANDLE_VALUE) {
      if (GetLastError() == ERROR_FILE_NOT_FOUND) {
        return true;
      }
      writeLogLine("WARN", "dumpFolderUnreadable error=%lu", GetLastError());
      return false;
    }
    do {
      totalBytes += fileBytes(foundFile);
      const unsigned long long writtenTime = fileTimeValue(foundFile.ftLastWriteTime);
      if (writtenTime < oldestTime && (keptName == nullptr || _stricmp(foundFile.cFileName, keptName) != 0)) {
        oldestTime = writtenTime;
        oldestBytes = fileBytes(foundFile);
        strcpy(oldestName, foundFile.cFileName);
      }
    } while (FindNextFileA(searchHandle, &foundFile));
    const DWORD enumerationError = GetLastError();
    FindClose(searchHandle);
    if (enumerationError != ERROR_NO_MORE_FILES) {
      writeLogLine("WARN", "dumpFolderUnreadable error=%lu", enumerationError);
      return false;
    }
    if (totalBytes <= maxDumpBytes || oldestName[0] == '\0') {
      return true;
    }
    char oldestPath[dumpPathBytes];
    snprintf(oldestPath, sizeof(oldestPath), "%s\\%s", dumpDirectory, oldestName);
    if (!DeleteFileA(oldestPath)) {
      writeLogLine("WARN", "dumpNotRemoved file=%s error=%lu", oldestName, GetLastError());
      return false;
    }
    writeLogLine("INFO", "dumpRemoved file=%s megabytes=%llu reason=sizeLimit", oldestName, oldestBytes / bytesPerMegabyte);
  }
}

bool writeRequestedDump() {
  SYSTEMTIME now;
  GetLocalTime(&now);
  char dumpName[MAX_PATH];
  snprintf(dumpName, sizeof(dumpName), "stabilityGuard_%04u%02u%02u_%02u%02u%02u_pid%lu_%s%lu.dmp", now.wYear, now.wMonth,
    now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentProcessId(), kindName(request.kind), request.number);
  char dumpPath[dumpPathBytes];
  snprintf(dumpPath, sizeof(dumpPath), "%s\\%s", dumpDirectory, dumpName);
  HANDLE dumpFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (dumpFile == INVALID_HANDLE_VALUE) {
    writeLogLine("WARN", "memoryDumpFailed kind=%s stage=create error=%lu", kindName(request.kind), GetLastError());
    return false;
  }
  MINIDUMP_EXCEPTION_INFORMATION exceptionInformation = { request.threadID, request.exception, FALSE };
  const DWORD startTicks = GetTickCount();
  const BOOL isWritten = miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile,
    request.kind == DumpKind::crash ? stackDumpType : fullDumpType, request.exception != nullptr ? &exceptionInformation : nullptr,
    nullptr, nullptr);
  const DWORD writeError = GetLastError();
  LARGE_INTEGER dumpBytes = {};
  GetFileSizeEx(dumpFile, &dumpBytes);
  CloseHandle(dumpFile);
  if (!isWritten) {
    DeleteFileA(dumpPath);
    writeLogLine("WARN", "memoryDumpFailed kind=%s stage=write error=0x%08lX", kindName(request.kind), writeError);
    return false;
  }
  writeLogLine("INFO", "memoryDumpWritten kind=%s file=%s megabytes=%llu milliseconds=%lu", kindName(request.kind), dumpName,
    static_cast<unsigned long long>(dumpBytes.QuadPart) / bytesPerMegabyte, GetTickCount() - startTicks);
  removeDumpsOverLimit(dumpName);
  return true;
}

DWORD WINAPI dumpThread(LPVOID) {
  for (;;) {
    if (WaitForSingleObject(requestEvent, INFINITE) != WAIT_OBJECT_0) {
      fatalError("The memory dump thread's wait failed (error %lu)", GetLastError());
    }
    request.isWritten = writeRequestedDump();
    SetEvent(doneEvent);
  }
}

// The game folder ships a 2003 dbghelp.dll that Windows would find first by name, so the system's copy is
// loaded by its full path, at startup rather than inside a crash.
void loadSystemDbgHelp() {
  char systemDirectory[MAX_PATH];
  const UINT systemDirectoryLength = GetSystemDirectoryA(systemDirectory, sizeof(systemDirectory));
  if (systemDirectoryLength == 0 || systemDirectoryLength >= sizeof(systemDirectory)) {
    fatalError("Could not find the Windows system directory (error %lu)", GetLastError());
  }
  char dbgHelpPath[MAX_PATH + 16];
  snprintf(dbgHelpPath, sizeof(dbgHelpPath), "%s\\dbghelp.dll", systemDirectory);
  HMODULE dbgHelp = LoadLibraryExA(dbgHelpPath, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (dbgHelp == nullptr) {
    fatalError("Could not load %s (error %lu)", dbgHelpPath, GetLastError());
  }
  miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFunction>(reinterpret_cast<void*>(GetProcAddress(dbgHelp, "MiniDumpWriteDump")));
  if (miniDumpWriteDump == nullptr) {
    fatalError("%s does not export MiniDumpWriteDump", dbgHelpPath);
  }
  char loadedPath[MAX_PATH];
  GetModuleFileNameA(dbgHelp, loadedPath, sizeof(loadedPath));
  writeLogLine("INFO", "dbgHelpLoaded path=%s", loadedPath);
}

}

void startMemoryDumps(const std::string& directory, unsigned long long limitBytes) {
  if (requestEvent != nullptr) {
    fatalError("startMemoryDumps called twice");
  }
  if (directory.size() >= sizeof(dumpDirectory)) {
    fatalError("The memory dump folder's path is too long");
  }
  strcpy(dumpDirectory, directory.c_str());
  maxDumpBytes = limitBytes;
  loadSystemDbgHelp();
  InitializeCriticalSection(&requestLock);
  requestEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  doneEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (requestEvent == nullptr || doneEvent == nullptr) {
    fatalError("Could not create the memory dump events (error %lu)", GetLastError());
  }
  if (!removeDumpsOverLimit(nullptr)) {
    fatalError("Could not keep the memory dumps under maxDumpGigabytes; the session log says why");
  }
  HANDLE thread = CreateThread(nullptr, 0, &dumpThread, nullptr, 0, &dumpThreadID);
  if (thread == nullptr) {
    fatalError("Could not start the memory dump thread (error %lu)", GetLastError());
  }
  CloseHandle(thread);
  writeLogLine("INFO", "memoryDumpsReady maxDumpGigabytes=%llu", maxDumpBytes / bytesPerGigabyte);
}

bool writeMemoryDump(DumpKind kind, EXCEPTION_POINTERS* exception, unsigned long number) {
  if (requestEvent == nullptr) {
    fatalError("writeMemoryDump called before startMemoryDumps");
  }
  if (isDumpThread()) {
    writeLogLine("WARN", "memoryDumpSkipped kind=%s reason=onDumpThread", kindName(kind));
    return false;
  }
  EnterCriticalSection(&requestLock);
  if (isDumpThreadStuck) {
    LeaveCriticalSection(&requestLock);
    writeLogLine("WARN", "memoryDumpSkipped kind=%s reason=dumpThreadStuck", kindName(kind));
    return false;
  }
  request = { kind, exception, GetCurrentThreadId(), number, false };
  SetEvent(requestEvent);
  const DWORD waitResult = WaitForSingleObject(doneEvent, dumpWaitMilliseconds);
  const bool isWritten = waitResult == WAIT_OBJECT_0 && request.isWritten;
  if (waitResult != WAIT_OBJECT_0) {
    isDumpThreadStuck = true;
    writeLogLine("WARN", "memoryDumpTimedOut kind=%s waitedMilliseconds=%lu", kindName(kind), dumpWaitMilliseconds);
  }
  LeaveCriticalSection(&requestLock);
  return isWritten;
}

bool isDumpThread() {
  return dumpThreadID != 0 && GetCurrentThreadId() == dumpThreadID;
}
