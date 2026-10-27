#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

// Log lines describing a thread, each event named <eventPrefix><What>: crashRegisters, freezeFrame,
// exitStackCandidate and so on. None of them allocate.

void writeRegisters(const char* eventPrefix, const CONTEXT* context);

uintptr_t contextStackPointer(const CONTEXT* context);
uintptr_t contextInstructionPointer(const CONTEXT* context);

// Unwinds the thread's frames from the context: on x64 with the executable's unwind tables (x64 code keeps no
// frame-pointer chain), on x86 along the frame-pointer chain (x86 has no unwind tables). Stops at the first frame
// that does not look like a real one (unreadable, not moving up the stack, or no code at the return address).
void writeFrameWalk(const char* eventPrefix, const CONTEXT* context);

// Copies up to maxSlots pointer-sized slots of a stack, starting at stackAddress and stopping where readable
// memory ends. Returns how many were copied.
size_t copyStackSlots(uintptr_t stackAddress, uintptr_t* slots, size_t maxSlots);

// Every copied slot that holds an address inside executable code. Some are stale, but callers in
// code the unwind tables do not cover only show up here.
void writeStackCandidates(const char* eventPrefix, const uintptr_t* slots, size_t slotCount);

void writeModuleList(const char* eventPrefix);
