#include "sessionLog.h"

#include "fatalError.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

static const char* const logsFolderName = "logs";
static const char* const logFileExtension = ".log";
static const unsigned long long retentionDays = 90;
static const unsigned long long fileTimeTicksPerDay = 24ULL * 60ULL * 60ULL * 10000000ULL;

static HANDLE sessionLogHandle = INVALID_HANDLE_VALUE;
// Hooks on any thread can log, so lines are written one at a time.
static CRITICAL_SECTION sessionLogLock;

static unsigned long long fileTimeToTicks(const FILETIME& fileTime) {
  return (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
}

void removeExpiredLogs(const std::string& logDirectory, const std::string& logFilePrefix) {
  FILETIME nowFileTime;
  GetSystemTimeAsFileTime(&nowFileTime);
  const unsigned long long expiryTicks = fileTimeToTicks(nowFileTime) - retentionDays * fileTimeTicksPerDay;

  const std::string searchPattern = logDirectory + "\\" + logFilePrefix + "*" + logFileExtension;
  WIN32_FIND_DATAA foundFile;
  HANDLE searchHandle = FindFirstFileA(searchPattern.c_str(), &foundFile);
  if (searchHandle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND) {
    return;
  }
  if (searchHandle == INVALID_HANDLE_VALUE) {
    fatalError("Could not enumerate the logs in %s (error %lu)", logDirectory.c_str(), GetLastError());
  }
  do {
    if (fileTimeToTicks(foundFile.ftLastWriteTime) >= expiryTicks) {
      continue;
    }
    const std::string expiredPath = logDirectory + "\\" + foundFile.cFileName;
    if (DeleteFileA(expiredPath.c_str())) {
      writeLogLine("INFO", "expiredLogRemoved file=%s", foundFile.cFileName);
    } else {
      writeLogLine("WARN", "expiredLogNotRemoved file=%s error=%lu", foundFile.cFileName, GetLastError());
    }
  } while (FindNextFileA(searchHandle, &foundFile));
  const DWORD enumerationError = GetLastError();
  FindClose(searchHandle);
  if (enumerationError != ERROR_NO_MORE_FILES) {
    fatalError("Listing the logs in %s stopped early (error %lu)", logDirectory.c_str(), enumerationError);
  }
}

std::string createLogFolder(const std::string& toolDirectory, const std::string& folderName) {
  const std::string logsDirectory = toolDirectory + "\\" + logsFolderName;
  const std::string logFolder = logsDirectory + "\\" + folderName;
  for (const std::string& directory : { logsDirectory, logFolder }) {
    if (!CreateDirectoryA(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
      fatalError("Could not create the log folder %s (error %lu)", directory.c_str(), GetLastError());
    }
  }
  return logFolder;
}

void openSessionLog(const std::string& toolDirectory, const char* toolName, const char* sessionStamp) {
  if (sessionLogHandle != INVALID_HANDLE_VALUE) {
    fatalError("openSessionLog called while a session log is already open");
  }

  const std::string logFilePrefix = std::string(toolName) + "_";
  const std::string logDirectory = createLogFolder(toolDirectory, toolName);

  char fileName[128];
  snprintf(fileName, sizeof(fileName), "%s%s_pid%lu%s", logFilePrefix.c_str(), sessionStamp, GetCurrentProcessId(),
    logFileExtension);

  const std::string logPath = logDirectory + "\\" + fileName;
  InitializeCriticalSection(&sessionLogLock);
  sessionLogHandle = CreateFileA(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (sessionLogHandle == INVALID_HANDLE_VALUE) {
    fatalError("Could not open session log %s (error %lu)", fileName, GetLastError());
  }

  removeExpiredLogs(logDirectory, logFilePrefix);
}

void closeSessionLog() {
  if (sessionLogHandle == INVALID_HANDLE_VALUE) {
    fatalError("closeSessionLog called while no session log is open");
  }
  EnterCriticalSection(&sessionLogLock);
  CloseHandle(sessionLogHandle);
  sessionLogHandle = INVALID_HANDLE_VALUE;
  LeaveCriticalSection(&sessionLogLock);
  DeleteCriticalSection(&sessionLogLock);
}

bool tryWriteLogLine(const char* level, const char* message) {
  if (sessionLogHandle == INVALID_HANDLE_VALUE) {
    return false;
  }
  SYSTEMTIME now;
  GetLocalTime(&now);
  char line[1280];
  const int lineLength = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u %s %s\r\n",
    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, level, message);
  if (lineLength < 0 || static_cast<size_t>(lineLength) >= sizeof(line)) {
    return false;
  }
  DWORD bytesWritten = 0;
  EnterCriticalSection(&sessionLogLock);
  const BOOL succeeded = WriteFile(sessionLogHandle, line, static_cast<DWORD>(lineLength), &bytesWritten, nullptr);
  LeaveCriticalSection(&sessionLogLock);
  return succeeded && bytesWritten == static_cast<DWORD>(lineLength);
}

bool isSessionLogHandle(HANDLE handle) {
  return handle == sessionLogHandle && sessionLogHandle != INVALID_HANDLE_VALUE;
}

void writeLogLine(const char* level, const char* format, ...) {
  char message[1024];
  va_list arguments;
  va_start(arguments, format);
  const int messageLength = vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  if (messageLength < 0 || static_cast<size_t>(messageLength) >= sizeof(message)) {
    fatalError("Log message did not fit the line buffer (format: %s)", format);
  }
  if (!tryWriteLogLine(level, message)) {
    fatalError("Could not write to the session log (error %lu)", GetLastError());
  }
}
