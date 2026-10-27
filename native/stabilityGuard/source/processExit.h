#pragma once

#include "configuration.h"

#include <windows.h>

// Logs every way the process ends itself, with the exit code and the calling thread's stack, so a game
// that quits without a crash still says where it quit from: ExitProcess (which the C runtime's exit and
// abort reach) and TerminateProcess on itself.
void installProcessExitLogging(const Configuration& configuration, DWORD mainThreadID);
