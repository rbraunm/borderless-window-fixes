#include "fatalError.h"

#include "sessionLog.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

static const char* fatalErrorTitle = "diagnostic";

void setFatalErrorTitle(const char* toolName) {
  fatalErrorTitle = toolName;
}

[[noreturn]] static void showAndTerminate(const char* logText, const char* message) {
  // The process is about to die, so a failed log write has nowhere to go.
  tryWriteLogLine("ERROR", logText);

  MessageBoxA(nullptr, message, fatalErrorTitle, MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
  TerminateProcess(GetCurrentProcess(), 1);
  for (;;) {
    Sleep(INFINITE);
  }
}

[[noreturn]] void fatalError(const char* format, ...) {
  char message[1024];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  message[sizeof(message) - 1] = '\0';
  showAndTerminate(message, message);
}

[[noreturn]] void privateFatalError(const char* logEvent, const char* format, ...) {
  char message[1024];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(message, sizeof(message), format, arguments);
  va_end(arguments);
  message[sizeof(message) - 1] = '\0';
  showAndTerminate(logEvent, message);
}
