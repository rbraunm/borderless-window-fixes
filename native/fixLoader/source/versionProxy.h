#pragma once

// X(exportName) for every export of the system version.dll.
#define versionExports(X) \
  X(GetFileVersionInfoA) \
  X(GetFileVersionInfoByHandle) \
  X(GetFileVersionInfoExA) \
  X(GetFileVersionInfoExW) \
  X(GetFileVersionInfoSizeA) \
  X(GetFileVersionInfoSizeExA) \
  X(GetFileVersionInfoSizeExW) \
  X(GetFileVersionInfoSizeW) \
  X(GetFileVersionInfoW) \
  X(VerFindFileA) \
  X(VerFindFileW) \
  X(VerInstallFileA) \
  X(VerInstallFileW) \
  X(VerLanguageNameA) \
  X(VerLanguageNameW) \
  X(VerQueryValueA) \
  X(VerQueryValueW)

// Loads the system copy of version.dll and points every proxy export at it.
// Called from DllMain, before the game's own code can call any of them.
void loadSystemVersionLibrary();
