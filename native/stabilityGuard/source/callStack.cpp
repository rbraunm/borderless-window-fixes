#include "callStack.h"

#include "addressNames.h"
#include "sessionLog.h"

#include <psapi.h>

static const size_t frameWalkLimit = 48;
static const uintptr_t frameSpanLimit = 1024 * 1024;

#if defined(_M_X64)
void writeRegisters(const char* eventPrefix, const CONTEXT* context) {
  writeLogLine("INFO", "%sRegisters rax=0x%016llX rbx=0x%016llX rcx=0x%016llX rdx=0x%016llX rsi=0x%016llX rdi=0x%016llX "
    "rbp=0x%016llX rsp=0x%016llX r8=0x%016llX r9=0x%016llX r10=0x%016llX r11=0x%016llX r12=0x%016llX r13=0x%016llX "
    "r14=0x%016llX r15=0x%016llX rip=0x%016llX eflags=0x%08lX", eventPrefix,
    context->Rax, context->Rbx, context->Rcx, context->Rdx, context->Rsi, context->Rdi, context->Rbp, context->Rsp,
    context->R8, context->R9, context->R10, context->R11, context->R12, context->R13, context->R14, context->R15,
    context->Rip, context->EFlags);
}

uintptr_t contextStackPointer(const CONTEXT* context) {
  return static_cast<uintptr_t>(context->Rsp);
}

uintptr_t contextInstructionPointer(const CONTEXT* context) {
  return static_cast<uintptr_t>(context->Rip);
}

// One unwind step. The unwinder reads the stack the context describes, which a crashed thread may have
// corrupted, so the read is guarded; false stops the walk.
static bool unwindFrame(CONTEXT* frame) {
  __try {
    DWORD64 imageBase = 0;
    RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(frame->Rip, &imageBase, nullptr);
    if (function == nullptr) {
      // A leaf function: the return address sits at the top of the stack.
      if (!isReadable(frame->Rsp, sizeof(DWORD64))) {
        return false;
      }
      frame->Rip = *reinterpret_cast<const DWORD64*>(frame->Rsp);
      frame->Rsp += sizeof(DWORD64);
      return true;
    }
    void* handlerData = nullptr;
    DWORD64 establisherFrame = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, frame->Rip, function, frame, &handlerData, &establisherFrame, nullptr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
#elif defined(_M_IX86)
void writeRegisters(const char* eventPrefix, const CONTEXT* context) {
  writeLogLine("INFO", "%sRegisters eax=0x%08lX ebx=0x%08lX ecx=0x%08lX edx=0x%08lX esi=0x%08lX edi=0x%08lX ebp=0x%08lX "
    "esp=0x%08lX eip=0x%08lX eflags=0x%08lX", eventPrefix, context->Eax, context->Ebx, context->Ecx, context->Edx,
    context->Esi, context->Edi, context->Ebp, context->Esp, context->Eip, context->EFlags);
}

uintptr_t contextStackPointer(const CONTEXT* context) {
  return static_cast<uintptr_t>(context->Esp);
}

uintptr_t contextInstructionPointer(const CONTEXT* context) {
  return static_cast<uintptr_t>(context->Eip);
}

// One step along the frame-pointer chain: [ebp] holds the caller's ebp and [ebp+4] the return address. Code built
// without frame pointers breaks the chain, which the stack candidates cover.
static bool unwindFrame(CONTEXT* frame) {
  const uintptr_t framePointer = static_cast<uintptr_t>(frame->Ebp);
  if (framePointer == 0 || !isReadable(framePointer, 2 * sizeof(DWORD))) {
    return false;
  }
  const DWORD* slots = reinterpret_cast<const DWORD*>(framePointer);
  frame->Ebp = slots[0];
  frame->Eip = slots[1];
  frame->Esp = static_cast<DWORD>(framePointer + 2 * sizeof(DWORD));
  return true;
}
#endif

void writeFrameWalk(const char* eventPrefix, const CONTEXT* context) {
  CONTEXT frame = *context;
  for (size_t frameIndex = 0; frameIndex < frameWalkLimit; frameIndex++) {
    const uintptr_t previousStack = contextStackPointer(&frame);
    if (!unwindFrame(&frame)) {
      break;
    }
    const uintptr_t returnAddress = contextInstructionPointer(&frame);
    if (returnAddress == 0 || !isExecutable(returnAddress)) {
      break;
    }
    char location[MAX_PATH + 32];
    describeAddress(returnAddress, location, sizeof(location));
    writeLogLine("INFO", "%sFrame index=%u returnAddress=0x%016llX location=%s", eventPrefix,
      static_cast<unsigned>(frameIndex), static_cast<unsigned long long>(returnAddress), location);
    if (contextStackPointer(&frame) <= previousStack || contextStackPointer(&frame) - previousStack > frameSpanLimit) {
      break;
    }
  }
}

size_t copyStackSlots(uintptr_t stackAddress, uintptr_t* slots, size_t maxSlots) {
  MEMORY_BASIC_INFORMATION region;
  if (VirtualQuery(reinterpret_cast<LPCVOID>(stackAddress), &region, sizeof(region)) != sizeof(region) ||
      region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return 0;
  }
  const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
  const size_t readableSlots = (regionEnd - stackAddress) / sizeof(uintptr_t);
  const size_t slotCount = readableSlots < maxSlots ? readableSlots : maxSlots;
  SIZE_T bytesRead = 0;
  if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(stackAddress), slots,
      slotCount * sizeof(uintptr_t), &bytesRead)) {
    return 0;
  }
  return bytesRead / sizeof(uintptr_t);
}

void writeStackCandidates(const char* eventPrefix, const uintptr_t* slots, size_t slotCount) {
  for (size_t slotIndex = 0; slotIndex < slotCount; slotIndex++) {
    if (!isExecutable(slots[slotIndex])) {
      continue;
    }
    char location[MAX_PATH + 32];
    describeAddress(slots[slotIndex], location, sizeof(location));
    writeLogLine("INFO", "%sStackCandidate rspOffset=0x%04llX value=0x%016llX location=%s", eventPrefix,
      static_cast<unsigned long long>(slotIndex * sizeof(uintptr_t)), static_cast<unsigned long long>(slots[slotIndex]), location);
  }
}

void writeModuleList(const char* eventPrefix) {
  HMODULE modules[512];
  DWORD bytesNeeded = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytesNeeded)) {
    writeLogLine("WARN", "%sModulesUnavailable error=%lu", eventPrefix, GetLastError());
    return;
  }
  const DWORD moduleCount = (bytesNeeded < sizeof(modules) ? bytesNeeded : sizeof(modules)) / sizeof(HMODULE);
  for (DWORD moduleIndex = 0; moduleIndex < moduleCount; moduleIndex++) {
    MODULEINFO moduleInfo;
    char modulePath[MAX_PATH];
    if (!K32GetModuleInformation(GetCurrentProcess(), modules[moduleIndex], &moduleInfo, sizeof(moduleInfo)) ||
        GetModuleFileNameA(modules[moduleIndex], modulePath, sizeof(modulePath)) == 0) {
      continue;
    }
    writeLogLine("INFO", "%sModule base=0x%016llX size=0x%08lX path=%s", eventPrefix,
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(moduleInfo.lpBaseOfDll)),
      static_cast<unsigned long>(moduleInfo.SizeOfImage), modulePath);
  }
}
