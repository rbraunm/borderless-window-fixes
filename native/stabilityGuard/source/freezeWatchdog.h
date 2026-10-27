#pragma once

#include "configuration.h"

#include <windows.h>

// Watches for the game freezing: its window not responding (Windows' own test, the one behind
// "Not Responding": the main thread has not looked at its messages for 5 seconds) for freezeSeconds
// more. Each freeze is logged once with where the main thread is stuck, and with writeMemoryDumps
// gets a dump of all the game's memory (startMemoryDumps first). Not while a debugger is attached.
void installFreezeWatchdog(const Configuration& configuration, DWORD mainThreadID);
