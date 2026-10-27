#include "entryPointHook.h"

#include "fatalError.h"

#include <windows.h>

#include <cstring>

typedef DWORD (WINAPI* EntryPointFunction)(LPVOID parameter);

// jmp qword ptr [rip+0] followed by the 8-byte target: reaches the detour wherever Windows placed this
// DLL, which a 32-bit relative jump would not with the game's image and the DLLs far apart.
static const size_t jumpLength = 14;
static BYTE* entryPointAddress = nullptr;
static BYTE savedEntryBytes[jumpLength];
static void (*entryCallback)() = nullptr;

static void writeCode(BYTE* target, const BYTE* bytes, size_t length) {
  DWORD previousProtection = 0;
  if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &previousProtection)) {
    fatalError("Could not unprotect the game entry point (error %lu)", GetLastError());
  }
  memcpy(target, bytes, length);
  DWORD ignoredProtection = 0;
  if (!VirtualProtect(target, length, previousProtection, &ignoredProtection)) {
    fatalError("Could not reprotect the game entry point (error %lu)", GetLastError());
  }
  if (!FlushInstructionCache(GetCurrentProcess(), target, length)) {
    fatalError("Could not flush the instruction cache (error %lu)", GetLastError());
  }
}

// Windows calls the entry point with one pointer argument; the detour takes the
// same shape, puts the original bytes back, runs the callback, and hands off.
// The game's C runtime startup ends the process itself and never returns here.
static DWORD WINAPI entryPointDetour(LPVOID parameter) {
  writeCode(entryPointAddress, savedEntryBytes, jumpLength);
  entryCallback();
  return reinterpret_cast<EntryPointFunction>(entryPointAddress)(parameter);
}

void installEntryPointHook(void (*onEntry)()) {
  if (entryCallback != nullptr) {
    fatalError("installEntryPointHook called twice");
  }
  entryCallback = onEntry;

  BYTE* executableBase = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
  const IMAGE_DOS_HEADER* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(executableBase);
  if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
    fatalError("The game executable has no DOS header");
  }
  const IMAGE_NT_HEADERS64* ntHeaders =
    reinterpret_cast<const IMAGE_NT_HEADERS64*>(executableBase + dosHeader->e_lfanew);
  if (ntHeaders->Signature != IMAGE_NT_SIGNATURE || ntHeaders->OptionalHeader.AddressOfEntryPoint == 0) {
    fatalError("The game executable has no entry point");
  }
  if (ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
    fatalError("The game executable is not a 64-bit (x64) program");
  }
  entryPointAddress = executableBase + ntHeaders->OptionalHeader.AddressOfEntryPoint;

  memcpy(savedEntryBytes, entryPointAddress, jumpLength);
  BYTE jump[jumpLength] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
  const void* detour = reinterpret_cast<const void*>(&entryPointDetour);
  memcpy(&jump[6], &detour, sizeof(detour));
  writeCode(entryPointAddress, jump, jumpLength);
}
