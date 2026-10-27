// The game imports VERSION.dll, and Windows looks for it in the game directory first, so this DLL is
// loaded as version.dll. Every export the real one has is reproduced (versionStubs.asm on x64, below on x86) as a
// jump through a pointer to the system copy.
#include "versionProxy.h"

#include "fatalError.h"

#include <windows.h>

#include <string>

// An export the system copy does not have (Wine omits some) resolves to this,
// so calling it fails loudly instead of jumping to address zero.
#define declareMissingExport(exportName) \
  extern "C" void missing##exportName() { \
    fatalError("The system version.dll does not export " #exportName ", but the game called it"); \
  }
versionExports(declareMissingExport)

// The pointers the stubs jump through.
#define declarePointer(exportName) \
  void* real##exportName = reinterpret_cast<void*>(&missing##exportName);
extern "C" {
versionExports(declarePointer)
}

#if defined(_M_IX86)
#define declareStub(exportName) \
  extern "C" __declspec(naked) void proxy##exportName() { \
    __asm { jmp dword ptr [real##exportName] } \
  }
versionExports(declareStub)
#endif

void loadSystemVersionLibrary() {
  char systemDirectory[MAX_PATH];
  const UINT systemDirectoryLength = GetSystemDirectoryA(systemDirectory, sizeof(systemDirectory));
  if (systemDirectoryLength == 0 || systemDirectoryLength >= sizeof(systemDirectory)) {
    fatalError("Could not find the Windows system directory (error %lu)", GetLastError());
  }
  // A full path, so the loader picks the system copy rather than this DLL again.
  const std::string systemVersionPath = std::string(systemDirectory) + "\\version.dll";
  HMODULE systemVersion = LoadLibraryA(systemVersionPath.c_str());
  if (systemVersion == nullptr) {
    fatalError("Could not load %s (error %lu)", systemVersionPath.c_str(), GetLastError());
  }

#define resolvePointer(exportName) \
  if (FARPROC resolved = GetProcAddress(systemVersion, #exportName)) { \
    real##exportName = reinterpret_cast<void*>(resolved); \
  }
  versionExports(resolvePointer)
#undef resolvePointer
}
