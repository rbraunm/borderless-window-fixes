#pragma once

#include "configuration.h"

#include <windows.h>

// Makes quitting reliable. A close request on one of the game's own top-level
// windows (the X button, Alt+F4), a click on its close button while the game is
// too busy to act on it (seen by a mouse hook on the watchdog's own thread), or the
// game starting to quit (PostQuitMessage on its main thread, where the in-game Quit button leads)
// arms a timer; if the process is still
// running when it expires, it is terminated, exactly as Task Manager would.
// Ctrl+Alt+F4 terminates it immediately, for a game too frozen to take input.
// Nothing changes while the game is running normally.
void installExitGuard(const Configuration& configuration, DWORD mainThreadID);
