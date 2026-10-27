#include "modulePath.h"

#include "fatalError.h"

// Long paths can make a module's path longer than MAX_PATH.
static const DWORD longestModulePath = 32768;

std::string resolveModuleDirectory(HMODULE module) {
  std::string path(MAX_PATH, '\0');
  for (;;) {
    const DWORD modulePathLength = GetModuleFileNameA(module, &path[0], static_cast<DWORD>(path.size()));
    if (modulePathLength == 0) {
      fatalError("Could not resolve the DLL path (error %lu)", GetLastError());
    }
    if (modulePathLength < path.size()) {
      path.resize(modulePathLength);
      break;
    }
    if (path.size() >= longestModulePath) {
      fatalError("The DLL path is longer than %lu characters", longestModulePath);
    }
    path.resize(path.size() * 2);
  }
  const size_t lastSeparator = path.find_last_of('\\');
  if (lastSeparator == std::string::npos) {
    fatalError("DLL path has no directory component");
  }
  return path.substr(0, lastSeparator);
}
