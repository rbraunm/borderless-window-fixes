#pragma once

// For a game that loads version.dll after its entry point has run, which a packed executable does while it unpacks:
// runs onStart on the loading thread the first time it calls LoadLibrary or GetProcAddress (the unpacker going on
// after loading this DLL) or one of the C runtime's startup calls (GetCommandLine, GetStartupInfo, GetVersion) without
// holding the loader lock. A game that makes none of them within lateStartSeconds is a fatal error. Called from DllMain
// with the hook registry initialized.
void installLateStart(void (*onStart)());

// The startup call that ran onStart, for the log.
const char* lateStartTrigger();
