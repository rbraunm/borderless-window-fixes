#pragma once

#include <windows.h>

#include <string>

enum class DumpKind {
  // An exception the game may survive: the stacks and the memory they point to.
  crash,
  // The game is ending in a crash: all of its memory.
  fatal,
  // The game stopped responding: all of its memory.
  freeze,
};

// Starts the thread that writes every memory dump. Dumps are never written by the thread that needs
// one, since a thread whose stack overflowed has no room left to write it. They go to dumpDirectory as
// stabilityGuard_*.dmp, and once they add up to more than maxDumpBytes (0: no limit) the oldest are
// removed, at start and after each new dump. Loads the system's dbghelp.dll.
void startMemoryDumps(const std::string& dumpDirectory, unsigned long long maxDumpBytes);

// Writes one dump of the whole process and waits for it. exception is null for a freeze. False when
// no dump was written; the session log says why.
bool writeMemoryDump(DumpKind kind, EXCEPTION_POINTERS* exception, unsigned long number);

// True on the thread that writes dumps; its own exceptions are not crashes to report.
bool isDumpThread();
