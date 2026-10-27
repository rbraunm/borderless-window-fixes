#pragma once

#include <string>

// The most stack slots a report copies; the copy is a fixed buffer on the reporting thread's stack.
const unsigned long stackScanSlotLimit = 1024;

struct Configuration {
  unsigned long maxFullReports;
  bool writeMemoryDumps;
  unsigned long long maxDumpBytes;
  unsigned long stackScanSlots;
  unsigned long freezeSeconds;
  bool exitGuard;
  unsigned long exitGraceSeconds;
  bool forceExitHotkey;
};

// Reads stabilityGuard.cfg from the windowFixes folder. Any problem is fatal.
Configuration loadConfiguration(const std::string& toolDirectory);
