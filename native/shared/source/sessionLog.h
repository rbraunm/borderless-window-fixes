#pragma once

#include <windows.h>

#include <string>

// Everything the tools generate goes under logs\ in the windowFixes folder (toolDirectory):
// logs\<folderName>\, created when missing, which this returns.
std::string createLogFolder(const std::string& toolDirectory, const std::string& folderName);

// Deletes the <logFilePrefix>*.log files in logDirectory last written before the retention period (90 days).
void removeExpiredLogs(const std::string& logDirectory, const std::string& logFilePrefix);

// Opens logs\<toolName>\<toolName>_<sessionStamp>_pid<processID>.log for this game session,
// appending when a reloaded tool reopens it, then removes that tool's session logs past
// the retention period.
void openSessionLog(const std::string& toolDirectory, const char* toolName, const char* sessionStamp);

// Closes the session log, for a tool that is being unloaded.
void closeSessionLog();

// Writes one timestamped line. Terminates the process if the write fails.
void writeLogLine(const char* level, const char* format, ...);

// Writes one timestamped line with a preformatted message. Returns false when
// no log is open or the write fails. Only fatalError should need this.
bool tryWriteLogLine(const char* level, const char* message);

// Lets the WriteFile hook recognize this DLL's own log writes and leave them untimed.
bool isSessionLogHandle(HANDLE handle);
