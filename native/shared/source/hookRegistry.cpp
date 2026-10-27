#include "hookRegistry.h"

#include "fatalError.h"
#include "sessionLog.h"

#include <MinHook.h>

#include <windows.h>

#include <cstdio>
#include <cstring>

struct HookedTarget {
  void* address;
  const char* name;
};

static const size_t hookedTargetCapacity = 64;
static HookedTarget hookedTargets[hookedTargetCapacity];
static size_t hookedTargetCount = 0;
static bool isInitialized = false;

static void requireMinHookSuccess(MH_STATUS status, const char* action, const char* targetName) {
  if (status != MH_OK) {
    fatalError("MinHook failed to %s %s: %s", action, targetName, MH_StatusToString(status));
  }
}

void initializeHookRegistry() {
  if (isInitialized) {
    fatalError("initializeHookRegistry called twice");
  }
  requireMinHookSuccess(MH_Initialize(), "initialize", "itself");
  isInitialized = true;
}

void* resolveLoadedExport(const char* moduleName, const char* exportName) {
  HMODULE module = GetModuleHandleA(moduleName);
  if (module == nullptr) {
    fatalError("%s is not loaded (error %lu)", moduleName, GetLastError());
  }
  void* address = reinterpret_cast<void*>(GetProcAddress(module, exportName));
  if (address == nullptr) {
    fatalError("%s does not export %s (error %lu)", moduleName, exportName, GetLastError());
  }
  return address;
}

bool installHook(const char* targetName, void* target, void* detour, void** original) {
  if (!isInitialized) {
    fatalError("installHook(%s) called before initializeHookRegistry", targetName);
  }
  for (size_t index = 0; index < hookedTargetCount; index++) {
    if (hookedTargets[index].address == target) {
      writeLogLine("INFO", "hookShared target=%s sharedWith=%s", targetName, hookedTargets[index].name);
      return false;
    }
  }
  if (hookedTargetCount == hookedTargetCapacity) {
    fatalError("Hook registry is full; raise hookedTargetCapacity");
  }
  requireMinHookSuccess(MH_CreateHook(target, detour, original), "create the hook for", targetName);
  requireMinHookSuccess(MH_EnableHook(target), "enable the hook for", targetName);
  hookedTargets[hookedTargetCount].address = target;
  hookedTargets[hookedTargetCount].name = targetName;
  hookedTargetCount++;
  writeLogLine("INFO", "hookInstalled target=%s", targetName);
  return true;
}

bool installExportHook(const char* moduleName, const char* exportName, void* detour, void** original) {
  const char* const dllSuffix = ".dll";
  const size_t moduleNameLength = strlen(moduleName);
  const size_t suffixLength = strlen(dllSuffix);
  const size_t baseLength = moduleNameLength > suffixLength && _stricmp(moduleName + moduleNameLength - suffixLength, dllSuffix) == 0
    ? moduleNameLength - suffixLength : moduleNameLength;
  char targetName[128];
  snprintf(targetName, sizeof(targetName), "%.*s!%s", static_cast<int>(baseLength), moduleName, exportName);
  return installHook(targetName, resolveLoadedExport(moduleName, exportName), detour, original);
}

static void registerTarget(void* address, const char* name) {
  for (size_t index = 0; index < hookedTargetCount; index++) {
    if (hookedTargets[index].address == address) {
      fatalError("%s patches code that %s already changed", name, hookedTargets[index].name);
    }
  }
  if (hookedTargetCount == hookedTargetCapacity) {
    fatalError("Hook registry is full; raise hookedTargetCapacity");
  }
  hookedTargets[hookedTargetCount].address = address;
  hookedTargets[hookedTargetCount].name = name;
  hookedTargetCount++;
}

void patchCode(const char* patchName, void* address, const unsigned char* expected, const unsigned char* replacement, size_t size) {
  if (memcmp(address, expected, size) != 0) {
    fatalError("%s: the game's code at %p is not what the patch expects", patchName, address);
  }
  registerTarget(address, patchName);
  DWORD previousProtection = 0;
  if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previousProtection)) {
    fatalError("%s: could not unprotect the game's code (error %lu)", patchName, GetLastError());
  }
  memcpy(address, replacement, size);
  DWORD ignoredProtection = 0;
  if (!VirtualProtect(address, size, previousProtection, &ignoredProtection)) {
    fatalError("%s: could not reprotect the game's code (error %lu)", patchName, GetLastError());
  }
  if (!FlushInstructionCache(GetCurrentProcess(), address, size)) {
    fatalError("%s: could not flush the instruction cache (error %lu)", patchName, GetLastError());
  }
  writeLogLine("INFO", "codePatched patch=%s address=%p", patchName, address);
}

void redirectCall(const char* patchName, void* callSite, void* expectedTarget, void* newTarget) {
  const unsigned char* const site = static_cast<const unsigned char*>(callSite);
  const auto relativeTo = [site](void* target) {
    return static_cast<long>(reinterpret_cast<const unsigned char*>(target) - (site + 5));
  };
  unsigned char expected[5] = { 0xE8 };
  unsigned char replacement[5] = { 0xE8 };
  const long expectedOffset = relativeTo(expectedTarget);
  const long newOffset = relativeTo(newTarget);
  memcpy(expected + 1, &expectedOffset, 4);
  memcpy(replacement + 1, &newOffset, 4);
  patchCode(patchName, callSite, expected, replacement, sizeof(expected));
}
