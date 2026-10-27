#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

// Writes "module.dll+0xOFFSET" for an address inside a loaded module, or
// "unmapped" when no module contains it. Never allocates, so it is safe to call
// from the exception handler.
void describeAddress(uintptr_t address, char* buffer, size_t bufferSize);

// True when the address is committed memory the process can read.
bool isReadable(uintptr_t address, size_t length);

// True when the address is committed memory marked executable.
bool isExecutable(uintptr_t address);
