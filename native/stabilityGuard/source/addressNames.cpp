#include "addressNames.h"

#include <cstdio>
#include <cstring>

void describeAddress(uintptr_t address, char* buffer, size_t bufferSize) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCSTR>(address), &module)) {
    snprintf(buffer, bufferSize, "unmapped");
    return;
  }
  char modulePath[MAX_PATH];
  const DWORD modulePathLength = GetModuleFileNameA(module, modulePath, sizeof(modulePath));
  const char* moduleName = "unknownModule";
  if (modulePathLength > 0 && modulePathLength < sizeof(modulePath)) {
    const char* lastSeparator = strrchr(modulePath, '\\');
    moduleName = lastSeparator != nullptr ? lastSeparator + 1 : modulePath;
  }
  snprintf(buffer, bufferSize, "%s+0x%llX", moduleName,
    static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
}

static bool queryRegion(uintptr_t address, MEMORY_BASIC_INFORMATION* region) {
  return VirtualQuery(reinterpret_cast<LPCVOID>(address), region, sizeof(*region)) == sizeof(*region) &&
    region->State == MEM_COMMIT && (region->Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

bool isReadable(uintptr_t address, size_t length) {
  MEMORY_BASIC_INFORMATION region;
  if (!queryRegion(address, &region)) {
    return false;
  }
  const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
  return address + length <= regionEnd;
}

bool isExecutable(uintptr_t address) {
  MEMORY_BASIC_INFORMATION region;
  if (!queryRegion(address, &region)) {
    return false;
  }
  return (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
