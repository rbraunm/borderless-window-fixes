#pragma once

#include <windows.h>

// Logs what the game does around losing and regaining focus, without changing any of it: the activation, focus,
// size and close messages its main thread's windows receive; every confinement, capture, visibility and raw input
// registration of the mouse and keyboard; the calls it makes into SDL2 for mouse grab, relative mouse mode,
// minimizing, fullscreen and hints, when SDL2.dll is loaded; and how steadily it presents OpenGL frames
// (SwapBuffers): every stall, frames per second for a while after each activation change, and a summary each minute.
void installFocusReporting(DWORD mainThreadID);
