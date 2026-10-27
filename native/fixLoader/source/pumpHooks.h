#pragma once

#include <windows.h>

// Hooks the message-pump calls (PeekMessageA/W, GetMessageA/W) so a tool can run work on the game's
// main thread. The hooks live in the loader, which is never unloaded, because the main thread spends
// much of its time inside them; a reloaded tool only removes and adds its callback.
void installPumpHooks(DWORD mainThreadID);

// ToolHost::addPumpCallback and ToolHost::removePumpCallback.
void addPumpCallback(void (*callback)());
void removePumpCallback(void (*callback)());
