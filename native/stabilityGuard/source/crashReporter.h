#pragma once

#include "configuration.h"

#include <windows.h>

// Reports crashes into the session log, and with writeMemoryDumps also as memory dumps (startMemoryDumps
// first). A first-chance handler reports each serious exception before the game's own handlers see it,
// up to maxFullReports distinct addresses, with a dump of the stacks; the game may still recover from
// these. An exception nothing handles is fatal: it is reported, and the process's first gets a dump of
// all its memory, whatever the report count, before the game's own unhandled-exception filter runs.
// Neither ever swallows an exception.
void installCrashReporter(const Configuration& configuration, DWORD mainThreadID);

// Writes the session's crash counts. Called once, at process detach.
void writeCrashTotals();
