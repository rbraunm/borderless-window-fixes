#include "fatalError.h"
#include "sessionLog.h"
#include "toolHost.h"

#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#include <cstdio>
#include <cstring>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

struct DllNotificationData {
  ULONG flags;
  const UNICODE_STRING* fullDllName;
  const UNICODE_STRING* baseDllName;
  PVOID dllBase;
  ULONG sizeOfImage;
};

typedef NTSTATUS (NTAPI* LdrGetDllHandleFunction)(PWSTR, PULONG, PUNICODE_STRING, PVOID*);
typedef VOID (CALLBACK* DllNotificationFunction)(ULONG reason, const DllNotificationData* data, PVOID context);
typedef NTSTATUS (NTAPI* LdrRegisterDllNotificationFunction)(ULONG, DllNotificationFunction, PVOID, PVOID*);
typedef NTSTATUS (NTAPI* LdrLockLoaderLockFunction)(ULONG, PULONG, PVOID*);
typedef NTSTATUS (NTAPI* LdrUnlockLoaderLockFunction)(ULONG, PVOID);

static const char* const toolName = "compatibilityUndo";
static const char* const compatibilityUndoVersion = "0.2.0";
// The DLLs Windows' compatibility layers (the Properties dialog's compatibility mode, or Steam's install script
// setting it) load into a program and point its imports at.
static const wchar_t* const shimModuleNames[] = { L"AcLayers.dll", L"AcGenral.dll", L"AcSpecfc.dll", L"AcXtrnal.dll",
  L"AcWinRT.dll" };
static const size_t shimModuleCount = sizeof(shimModuleNames) / sizeof(shimModuleNames[0]);
static const ULONG dllLoadedReason = 1;
static const int forwarderDepthLimit = 8;

struct ShimModule {
  const wchar_t* name;
  uintptr_t base;
  uintptr_t end;
};

static bool isLoaded = false;
static LdrGetDllHandleFunction ldrGetDllHandle = nullptr;
static LdrLockLoaderLockFunction ldrLockLoaderLock = nullptr;
static LdrUnlockLoaderLockFunction ldrUnlockLoaderLock = nullptr;
static ShimModule shimModules[shimModuleCount];
static size_t loadedShimCount = 0;
static volatile LONG isSweepPending = 0;

static void* requireNtdllExport(HMODULE ntdll, const char* name) {
  void* address = reinterpret_cast<void*>(GetProcAddress(ntdll, name));
  if (address == nullptr) {
    fatalError("ntdll.dll does not export %s", name);
  }
  return address;
}

static HMODULE findLoadedModule(const wchar_t* name) {
  UNICODE_STRING moduleName;
  RtlInitUnicodeString(&moduleName, name);
  PVOID handle = nullptr;
  return ldrGetDllHandle(nullptr, nullptr, &moduleName, &handle) >= 0 ? static_cast<HMODULE>(handle) : nullptr;
}

static HMODULE requireLoadedModule(const char* name) {
  wchar_t wideName[MAX_PATH];
  if (MultiByteToWideChar(CP_ACP, 0, name, -1, wideName, MAX_PATH) == 0) {
    fatalError("DLL name %s does not convert (error %lu)", name, GetLastError());
  }
  HMODULE module = findLoadedModule(wideName);
  if (module == nullptr) {
    fatalError("%s is imported but not loaded", name);
  }
  return module;
}

static void findShimModules() {
  loadedShimCount = 0;
  for (size_t index = 0; index < shimModuleCount; index++) {
    HMODULE module = findLoadedModule(shimModuleNames[index]);
    if (module == nullptr) {
      continue;
    }
    MODULEINFO information;
    if (!K32GetModuleInformation(GetCurrentProcess(), module, &information, sizeof(information))) {
      fatalError("GetModuleInformation failed for a compatibility layer module (error %lu)", GetLastError());
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(information.lpBaseOfDll);
    shimModules[loadedShimCount++] = { shimModuleNames[index], base, base + information.SizeOfImage };
  }
}

static const ShimModule* findShim(uintptr_t address) {
  for (size_t index = 0; index < loadedShimCount; index++) {
    if (address >= shimModules[index].base && address < shimModules[index].end) {
      return &shimModules[index];
    }
  }
  return nullptr;
}

// Reads the module's export table itself: the layers also answer GetProcAddress and LdrGetProcedureAddress with their
// own replacements. A forwarded export ("NTDLL.RtlFreeHeap") is followed into the module it names.
static void* findExport(HMODULE module, const char* name, ULONG ordinal, const char* importName, int depth) {
  if (depth > forwarderDepthLimit) {
    fatalError("%s forwards more than %d times", importName, forwarderDepthLimit);
  }
  BYTE* base = reinterpret_cast<BYTE*>(module);
  const IMAGE_NT_HEADERS* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
    base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const IMAGE_DATA_DIRECTORY& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (directory.VirtualAddress == 0) {
    fatalError("%s: the module has no export table", importName);
  }
  const IMAGE_EXPORT_DIRECTORY* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
  const DWORD* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
  DWORD functionIndex = MAXDWORD;
  if (name == nullptr) {
    functionIndex = ordinal - exports->Base;
  } else {
    const DWORD* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const WORD* nameOrdinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
    for (DWORD index = 0; index < exports->NumberOfNames; index++) {
      if (strcmp(reinterpret_cast<const char*>(base + names[index]), name) == 0) {
        functionIndex = nameOrdinals[index];
        break;
      }
    }
  }
  if (functionIndex >= exports->NumberOfFunctions || functions[functionIndex] == 0) {
    fatalError("%s is not exported", importName);
  }
  const DWORD functionRVA = functions[functionIndex];
  if (functionRVA < directory.VirtualAddress || functionRVA >= directory.VirtualAddress + directory.Size) {
    return base + functionRVA;
  }
  const char* forwarder = reinterpret_cast<const char*>(base + functionRVA);
  const char* separator = strrchr(forwarder, '.');
  if (separator == nullptr || separator == forwarder) {
    fatalError("%s forwards to malformed %s", importName, forwarder);
  }
  char moduleName[MAX_PATH];
  snprintf(moduleName, sizeof(moduleName), "%.*s.dll", static_cast<int>(separator - forwarder), forwarder);
  HMODULE target = requireLoadedModule(moduleName);
  if (separator[1] == '#') {
    return findExport(target, nullptr, strtoul(separator + 2, nullptr, 10), importName, depth + 1);
  }
  return findExport(target, separator + 1, 0, importName, depth + 1);
}

static void* resolveImport(const char* dllName, const IMAGE_THUNK_DATA* nameThunk, BYTE* base, char* importName,
    size_t importNameSize) {
  HMODULE module = requireLoadedModule(dllName);
  if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) {
    const ULONG ordinal = static_cast<ULONG>(IMAGE_ORDINAL(nameThunk->u1.Ordinal));
    snprintf(importName, importNameSize, "%s!#%lu", dllName, ordinal);
    return findExport(module, nullptr, ordinal, importName, 0);
  }
  const IMAGE_IMPORT_BY_NAME* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + nameThunk->u1.AddressOfData);
  snprintf(importName, importNameSize, "%s!%s", dllName, byName->Name);
  return findExport(module, byName->Name, 0, importName, 0);
}

static void writeImport(IMAGE_THUNK_DATA* slot, void* target) {
  DWORD previousProtection = 0;
  if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previousProtection)) {
    fatalError("Could not unprotect an import table entry (error %lu)", GetLastError());
  }
  slot->u1.Function = reinterpret_cast<uintptr_t>(target);
  DWORD ignoredProtection = 0;
  if (!VirtualProtect(slot, sizeof(*slot), previousProtection, &ignoredProtection)) {
    fatalError("Could not reprotect an import table entry (error %lu)", GetLastError());
  }
}

// Points every import of the module that a compatibility layer redirected back at the function it names.
static size_t restoreModuleImports(HMODULE module, const char* moduleName, const char* trigger) {
  BYTE* base = reinterpret_cast<BYTE*>(module);
  const IMAGE_NT_HEADERS* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
    base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const IMAGE_DATA_DIRECTORY& directory = headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (directory.VirtualAddress == 0) {
    return 0;
  }
  size_t restored = 0;
  for (const IMAGE_IMPORT_DESCRIPTOR* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
      base + directory.VirtualAddress); descriptor->Name != 0; descriptor++) {
    const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
    IMAGE_THUNK_DATA* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
    // Some linkers leave no name table, or point it at the address table Windows overwrites.
    const IMAGE_THUNK_DATA* nameThunks = descriptor->OriginalFirstThunk != 0 &&
      descriptor->OriginalFirstThunk != descriptor->FirstThunk ?
      reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk) : nullptr;
    for (size_t index = 0; slots[index].u1.Function != 0; index++) {
      const ShimModule* shim = findShim(static_cast<uintptr_t>(slots[index].u1.Function));
      if (shim == nullptr) {
        continue;
      }
      if (nameThunks == nullptr || (!IMAGE_SNAP_BY_ORDINAL(nameThunks[index].u1.Ordinal) &&
          nameThunks[index].u1.AddressOfData >= headers->OptionalHeader.SizeOfImage)) {
        writeLogLine("WARN", "importUnrestorable trigger=%s module=%s dll=%s index=%zu shim=%ls+0x%lX reason=noNameTable",
          trigger, moduleName, dllName, index, shim->name, static_cast<unsigned long>(slots[index].u1.Function - shim->base));
        continue;
      }
      char importName[MAX_PATH + 64];
      void* target = resolveImport(dllName, &nameThunks[index], base, importName, sizeof(importName));
      if (findShim(reinterpret_cast<uintptr_t>(target)) != nullptr) {
        fatalError("%s resolves into the compatibility layer itself", importName);
      }
      writeLogLine("INFO", "importRestored trigger=%s module=%s import=%s shim=%ls+0x%lX target=%p", trigger, moduleName,
        importName, shim->name, static_cast<unsigned long>(slots[index].u1.Function - shim->base), target);
      writeImport(&slots[index], target);
      restored++;
    }
  }
  return restored;
}

// Runs with the loader lock held, so no module loads or unloads while the import tables are read and written.
static void sweepImports(const char* trigger) {
  findShimModules();
  if (loadedShimCount == 0) {
    return;
  }
  HMODULE modules[1024];
  DWORD bytesNeeded = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytesNeeded)) {
    fatalError("EnumProcessModules failed (error %lu)", GetLastError());
  }
  if (bytesNeeded > sizeof(modules)) {
    fatalError("More than %zu modules are loaded", sizeof(modules) / sizeof(modules[0]));
  }
  size_t restored = 0;
  for (DWORD index = 0; index < bytesNeeded / sizeof(HMODULE); index++) {
    if (findShim(reinterpret_cast<uintptr_t>(modules[index])) != nullptr) {
      continue;
    }
    char modulePath[MAX_PATH];
    if (GetModuleFileNameA(modules[index], modulePath, sizeof(modulePath)) == 0) {
      fatalError("GetModuleFileName failed (error %lu)", GetLastError());
    }
    const char* moduleName = strrchr(modulePath, '\\');
    restored += restoreModuleImports(modules[index], moduleName != nullptr ? moduleName + 1 : modulePath, trigger);
  }
  if (restored != 0) {
    writeLogLine("INFO", "compatibilityUndone trigger=%s imports=%zu", trigger, restored);
  }
}

// A layer redirects the imports of a DLL as it loads, and can add redirections to modules already loaded. Sweeping in
// the notification catches what it did before the notification; the sweep on the main thread's next message pump
// catches what it did after.
static VOID CALLBACK onDllNotification(ULONG reason, const DllNotificationData* data, PVOID context) {
  (void)data;
  (void)context;
  if (reason == dllLoadedReason) {
    sweepImports("dllLoaded");
    InterlockedExchange(&isSweepPending, 1);
  }
}

static void onPump() {
  if (InterlockedExchange(&isSweepPending, 0) == 0) {
    return;
  }
  PVOID cookie = nullptr;
  if (ldrLockLoaderLock(0, nullptr, &cookie) < 0) {
    fatalError("LdrLockLoaderLock failed");
  }
  sweepImports("pump");
  if (ldrUnlockLoaderLock(0, cookie) < 0) {
    fatalError("LdrUnlockLoaderLock failed");
  }
}

extern "C" {

// Windows' compatibility layers redirect a program's imports into their own replacements, and those replacements
// can break an old program on today's Windows (Jade Empire's configuration program dies in the XP layer's HeapFree).
// This points every redirected import of every loaded module back at the real function, at load and again after
// every DLL load. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", compatibilityUndoVersion,
    windowFixesBuildCommit, GetCurrentThreadId());
  HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  if (ntdll == nullptr) {
    fatalError("ntdll.dll is not loaded (error %lu)", GetLastError());
  }
  ldrGetDllHandle = reinterpret_cast<LdrGetDllHandleFunction>(requireNtdllExport(ntdll, "LdrGetDllHandle"));
  ldrLockLoaderLock = reinterpret_cast<LdrLockLoaderLockFunction>(requireNtdllExport(ntdll, "LdrLockLoaderLock"));
  ldrUnlockLoaderLock = reinterpret_cast<LdrUnlockLoaderLockFunction>(requireNtdllExport(ntdll, "LdrUnlockLoaderLock"));
  LdrRegisterDllNotificationFunction ldrRegisterDllNotification = reinterpret_cast<LdrRegisterDllNotificationFunction>(
    requireNtdllExport(ntdll, "LdrRegisterDllNotification"));

  findShimModules();
  if (loadedShimCount == 0) {
    writeLogLine("INFO", "compatibilityLayerAbsent");
    writeLogLine("INFO", "loadComplete");
    return 0;
  }
  for (size_t index = 0; index < loadedShimCount; index++) {
    writeLogLine("INFO", "compatibilityModule name=%ls base=0x%p", shimModules[index].name,
      reinterpret_cast<void*>(shimModules[index].base));
  }
  PVOID cookie = nullptr;
  if (ldrLockLoaderLock(0, nullptr, &cookie) < 0) {
    fatalError("LdrLockLoaderLock failed");
  }
  sweepImports("load");
  if (ldrUnlockLoaderLock(0, cookie) < 0) {
    fatalError("LdrUnlockLoaderLock failed");
  }
  PVOID notificationCookie = nullptr;
  const NTSTATUS status = ldrRegisterDllNotification(0, &onDllNotification, nullptr, &notificationCookie);
  if (status < 0) {
    fatalError("LdrRegisterDllNotification failed (status 0x%08lX)", static_cast<unsigned long>(status));
  }
  host->setPumpCallback(&onPump);
  writeLogLine("INFO", "loadComplete");
  return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

}
