#pragma once

#include <windows.h>

// What fixLoader hands each tool's Load. Every tool exports
//   DWORD Load(const ToolHost* host)
// and a tool that also exports
//   DWORD Unload(void)
// can be reloaded while the game runs: Unload removes everything the tool installed, stops its
// threads, and returns once no thread can run the tool's code again, so the loader can free it.
// Tools load from shadow copies under <toolDirectory>\loaded, so a build can replace any tool's DLL
// while the game holds it.
struct ToolHost {
  // The windowFixes folder: the tools, their settings files, the plugins folder and the log folders.
  const char* toolDirectory;
  // Names this game session in every tool's log file name; the same across reloads.
  const char* sessionStamp;
  DWORD mainThreadID;
  // Runs callback on the game's main thread each time it pumps window messages (PeekMessage,
  // GetMessage); nullptr stops it. Clearing returns once a run in progress has returned. One
  // callback at a time.
  void (*setPumpCallback)(void (*callback)());
};

typedef DWORD (*ToolLoadFunction)(const ToolHost* host);
typedef DWORD (*ToolUnloadFunction)(void);
