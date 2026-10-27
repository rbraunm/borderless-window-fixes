#pragma once

// Diverts the game executable's entry point so onEntry runs on the main thread
// after every statically imported DLL has initialized but before any game code
// (its C runtime startup included), then lets the game start normally.
// Called from DllMain; writes one jump over the entry point's first bytes.
void installEntryPointHook(void (*onEntry)());
