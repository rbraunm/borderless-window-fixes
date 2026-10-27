#pragma once

// Names the tool in the fatal error message box. Call first thing in Load.
void setFatalErrorTitle(const char* toolName);

// Records the message in the session log (when one is open), shows it in a
// message box, and terminates the process. Never returns.
[[noreturn]] void fatalError(const char* format, ...);

// For messages that name an account, a character or a credential: the log gets only logEvent, the
// message box the full text. Never returns.
[[noreturn]] void privateFatalError(const char* logEvent, const char* format, ...);
