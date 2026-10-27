#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"
#include "toolHost.h"

#include <windows.h>

#include <cstring>
#include <unordered_map>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

typedef void (__thiscall* PhysicsStepFunction)(void* object);

static const char* const toolName = "physicsTimestep";
static const char* const physicsTimestepVersion = "0.1.0";
static const char* const gameProgramName = "JadeEmpire.exe";
// The Steam JadeEmpire.exe (link time 0x45B6777A), unpacked: per object, the forces step (max(frame time, 1/30 s))
// then the integration step ((1/30 s) squared), called back to back once per frame from two object lists.
static void* const forcesStepAddress = reinterpret_cast<void*>(0x00604550);
static void* const integrateStepAddress = reinterpret_cast<void*>(0x00604B50);
static const unsigned char forcesStepPrologue[] = { 0x83, 0xEC, 0x60, 0xD9, 0x05, 0xD0, 0xEC, 0x75, 0x00 };
static const unsigned char integrateStepPrologue[] = { 0x83, 0xEC, 0x50, 0x53, 0x55, 0x56, 0x8B, 0xF1 };
static const LONGLONG stepsPerSecond = 30;
static const size_t prunedObjectCount = 4096;
static const LONGLONG summaryMilliseconds = 60000;

static bool isLoaded = false;
static bool isHooked = false;
static DWORD gameMainThreadID = 0;
static PhysicsStepFunction originalForcesStep = nullptr;
static PhysicsStepFunction originalIntegrateStep = nullptr;
static LONGLONG counterFrequency = 0;
static LONGLONG stepPeriod = 0;
static std::unordered_map<void*, LONGLONG> nextStepOf;
static void* steppingObject = nullptr;
static LONGLONG summaryStart = 0;
static unsigned long stepsRun = 0;
static unsigned long stepsSkipped = 0;

static LONGLONG counterNow() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

static void requireMainThread(const char* function) {
  if (GetCurrentThreadId() != gameMainThreadID) {
    fatalError("%s ran on thread %lu, not the game's main thread %lu", function, GetCurrentThreadId(), gameMainThreadID);
  }
}

static void logSummary(LONGLONG now) {
  if (summaryStart == 0) {
    summaryStart = now;
    return;
  }
  if ((now - summaryStart) * 1000 / counterFrequency < summaryMilliseconds) {
    return;
  }
  writeLogLine("INFO", "stepSummary seconds=%lld run=%lu skipped=%lu objects=%zu", (now - summaryStart) / counterFrequency,
    stepsRun, stepsSkipped, nextStepOf.size());
  summaryStart = now;
  stepsRun = 0;
  stepsSkipped = 0;
}

// An object steps when its next 1/30 s slot has come; one that fell more than a slot behind starts its slots over
// rather than stepping several times to catch up. Objects come and go, so slots older than a second are dropped.
static void __fastcall forcesStepDetour(void* object, void* unusedEdx) {
  (void)unusedEdx;
  requireMainThread("The forces step");
  const LONGLONG now = counterNow();
  logSummary(now);
  if (nextStepOf.size() > prunedObjectCount) {
    for (auto entry = nextStepOf.begin(); entry != nextStepOf.end();) {
      entry = now - entry->second > counterFrequency ? nextStepOf.erase(entry) : std::next(entry);
    }
  }
  LONGLONG& nextStep = nextStepOf[object];
  if (nextStep != 0 && now < nextStep) {
    steppingObject = nullptr;
    stepsSkipped++;
    return;
  }
  nextStep = nextStep == 0 || now - nextStep > stepPeriod ? now + stepPeriod : nextStep + stepPeriod;
  steppingObject = object;
  stepsRun++;
  originalForcesStep(object);
}

static void __fastcall integrateStepDetour(void* object, void* unusedEdx) {
  (void)unusedEdx;
  requireMainThread("The integration step");
  if (object != steppingObject) {
    return;
  }
  steppingObject = nullptr;
  originalIntegrateStep(object);
}

static void hookStep(const char* name, void* address, const unsigned char* prologue, size_t prologueSize, void* detour,
    PhysicsStepFunction* original) {
  if (memcmp(address, prologue, prologueSize) != 0) {
    fatalError("%s at %p is not the code this tool was written for: this is a different JadeEmpire.exe", name, address);
  }
  installHook(name, address, detour, reinterpret_cast<void**>(original));
}

// The code is still packed while the tools load; by the first message pump the game is running unpacked.
static void onPump() {
  if (isHooked) {
    return;
  }
  isHooked = true;
  hookStep("JadeEmpire!forcesStep", forcesStepAddress, forcesStepPrologue, sizeof(forcesStepPrologue),
    reinterpret_cast<void*>(&forcesStepDetour), &originalForcesStep);
  hookStep("JadeEmpire!integrateStep", integrateStepAddress, integrateStepPrologue, sizeof(integrateStepPrologue),
    reinterpret_cast<void*>(&integrateStepDetour), &originalIntegrateStep);
}

extern "C" {

// The game simulates its physics objects (cloth among them) one fixed 1/30 s step per frame, which was right at its
// own 30 frames per second cap and runs them faster the higher the frame rate. This lets each object step at most once
// per 1/30 s of real time, so they move at their designed speed at any frame rate. In the configuration program it
// does nothing. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", physicsTimestepVersion,
    windowFixesBuildCommit, GetCurrentThreadId());
  char executablePath[MAX_PATH];
  const DWORD length = GetModuleFileNameA(nullptr, executablePath, sizeof(executablePath));
  if (length == 0 || length >= sizeof(executablePath)) {
    fatalError("Could not resolve the executable path (error %lu)", GetLastError());
  }
  const char* executableName = strrchr(executablePath, '\\') + 1;
  if (_stricmp(executableName, gameProgramName) != 0) {
    writeLogLine("INFO", "notGameProgram executable=%s", executableName);
    writeLogLine("INFO", "loadComplete");
    return 0;
  }
  gameMainThreadID = host->mainThreadID;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  counterFrequency = frequency.QuadPart;
  stepPeriod = counterFrequency / stepsPerSecond;
  initializeHookRegistry();
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
