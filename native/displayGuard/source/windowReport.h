#pragma once

#include <windows.h>

// Logs what the game does with its display, without changing any of it: every top-level window its main
// thread creates (class, title, styles, placement), each later change of their style or placement, every
// display mode change it asks for, and which graphics core DLL it loads.
void installWindowReporting(DWORD mainThreadID);
