#include <windows.h>

#include "physicsSteps.h"

#include "fatalError.h"
#include "gameClock.h"
#include "hookRegistry.h"
#include "sessionLog.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

typedef void (__thiscall* PhysicsStepFunction)(void* object);

static const double originalStepsPerSecond = 30.0;
// One frame catches up at most this much game time, as the game caps its own frame time; an object further behind
// starts its steps over.
static const double longestCatchUpSeconds = 0.25;
static const double staleObjectSeconds = 1.0;
static const size_t prunedObjectCount = 4096;
static const double summarySeconds = 60.0;

// The Steam JadeEmpire.exe (link time 0x45B6777A), unpacked. Per physics object, once per frame and back to back: the
// forces step, which steps by max(world frame time, 1/30 s), and the integration, which steps by 1/30 s times the
// object's time scale and keeps 1 - damping of each point's motion per step.
static void* const forcesStepAddress = reinterpret_cast<void*>(0x00604550);
static void* const integrateStepAddress = reinterpret_cast<void*>(0x00604B50);
static const unsigned char forcesStepPrologue[] = { 0x83, 0xEC, 0x60, 0xD9, 0x05, 0xD0, 0xEC, 0x75, 0x00 };
static const unsigned char integrateStepPrologue[] = { 0x83, 0xEC, 0x50, 0x53, 0x55, 0x56, 0x8B, 0xF1 };
// jne to the 1/30 s branch of max(), and the frame time it loads otherwise.
static unsigned char* const forcesMaxBranchAddress = reinterpret_cast<unsigned char*>(0x0060456E);
static const unsigned char forcesMaxBranch[] = { 0x75, 0x0B };
static unsigned char* const forcesStepLoadAddress = reinterpret_cast<unsigned char*>(0x00604570);
static const unsigned char forcesStepLoad[] = { 0xA1, 0xD0, 0xEC, 0x75, 0x00 };
// fld 1/30 s, and fsubr 1.0 (the share of motion kept).
static unsigned char* const integrateStepLoadAddress = reinterpret_cast<unsigned char*>(0x00604B79);
static const unsigned char integrateStepLoad[] = { 0xD9, 0x05, 0xF8, 0xFC, 0x6E, 0x00 };
static unsigned char* const integrateKeepAddress = reinterpret_cast<unsigned char*>(0x00604BA8);
static const unsigned char integrateKeep[] = { 0xD8, 0x2D, 0x10, 0x19, 0x6E, 0x00 };

static DWORD gameMainThreadID = 0;
static double stepSecondsWanted = 0.0;
static PhysicsStepFunction originalForcesStep = nullptr;
static PhysicsStepFunction originalIntegrateStep = nullptr;
// What the patched code reads: the step in seconds, and the power the kept share of motion is raised to.
static float stepSeconds = 0.0f;
static double keepExponent = 1.0;
static std::unordered_map<void*, double> nextStepOf;
static double summaryStart = -1.0;
static unsigned long stepsRun = 0;

// The share of each point's motion one step keeps, scaled so the share kept per second is the original game's.
static float __cdecl scaledKeep(float damping) {
  const double keep = 1.0 - damping;
  return keep > 0.0 ? static_cast<float>(pow(keep, keepExponent)) : static_cast<float>(keep);
}

// Replaces fsubr 1.0 with st0 = scaledKeep(st0); the code after it keeps nothing in eax, ecx or edx.
static __declspec(naked) void keepHelper() {
  __asm {
    sub esp, 4
    fstp dword ptr [esp]
    call scaledKeep
    add esp, 4
    ret
  }
}

static void logSummary(double now) {
  if (summaryStart < 0.0) {
    summaryStart = now;
    return;
  }
  if (now - summaryStart < summarySeconds) {
    return;
  }
  writeLogLine("INFO", "stepSummary gameSeconds=%.0f steps=%lu objects=%zu", now - summaryStart, stepsRun, nextStepOf.size());
  summaryStart = now;
  stepsRun = 0;
}

static void runStep(void* object) {
  stepSeconds = static_cast<float>(stepSecondsWanted);
  keepExponent = stepSecondsWanted * originalStepsPerSecond;
  originalForcesStep(object);
  originalIntegrateStep(object);
  stepsRun++;
}

// Takes over the pair: the game calls the forces step once per frame, this runs as many whole fixed steps (forces then
// integration) as the object's game time calls for, none to several, and the game's own integration call that follows
// does nothing.
static void __fastcall forcesStepDetour(void* object, void* unusedEdx) {
  (void)unusedEdx;
  if (GetCurrentThreadId() != gameMainThreadID) {
    fatalError("The forces step ran on thread %lu, not the game's main thread %lu", GetCurrentThreadId(), gameMainThreadID);
  }
  const double now = gameTimeNow();
  logSummary(now);
  if (nextStepOf.size() > prunedObjectCount) {
    for (auto entry = nextStepOf.begin(); entry != nextStepOf.end();) {
      entry = now - entry->second > staleObjectSeconds ? nextStepOf.erase(entry) : std::next(entry);
    }
  }
  auto found = nextStepOf.find(object);
  if (found == nextStepOf.end() || now - found->second > longestCatchUpSeconds) {
    found = nextStepOf.insert_or_assign(object, now).first;
  }
  while (found->second <= now) {
    runStep(object);
    found->second += stepSecondsWanted;
  }
}

static void __fastcall integrateStepDetour(void* object, void* unusedEdx) {
  (void)object;
  (void)unusedEdx;
}

static void* addressOfStep() {
  return &stepSeconds;
}

void installPhysicsSteps(unsigned long stepsPerSecond, DWORD mainThreadID) {
  gameMainThreadID = mainThreadID;
  stepSecondsWanted = 1.0 / static_cast<double>(stepsPerSecond);
  if (memcmp(forcesStepAddress, forcesStepPrologue, sizeof(forcesStepPrologue)) != 0 ||
      memcmp(integrateStepAddress, integrateStepPrologue, sizeof(integrateStepPrologue)) != 0) {
    fatalError("The physics steps are not the code this tool was written for: this is a different JadeEmpire.exe");
  }
  const DWORD stepAddress = static_cast<DWORD>(reinterpret_cast<uintptr_t>(addressOfStep()));
  const unsigned char alwaysFrameTime[] = { 0x90, 0x90 };
  patchCode("JadeEmpire!forcesStepMax", forcesMaxBranchAddress, forcesMaxBranch, alwaysFrameTime, sizeof(alwaysFrameTime));
  unsigned char loadStep[sizeof(forcesStepLoad)] = { 0xA1 };
  memcpy(&loadStep[1], &stepAddress, sizeof(stepAddress));
  patchCode("JadeEmpire!forcesStepLoad", forcesStepLoadAddress, forcesStepLoad, loadStep, sizeof(loadStep));
  unsigned char integrateLoad[sizeof(integrateStepLoad)] = { 0xD9, 0x05 };
  memcpy(&integrateLoad[2], &stepAddress, sizeof(stepAddress));
  patchCode("JadeEmpire!integrateStepLoad", integrateStepLoadAddress, integrateStepLoad, integrateLoad, sizeof(integrateLoad));
  unsigned char keepCall[sizeof(integrateKeep)] = { 0xE8, 0, 0, 0, 0, 0x90 };
  const LONG keepOffset = static_cast<LONG>(reinterpret_cast<uintptr_t>(&keepHelper) -
    reinterpret_cast<uintptr_t>(integrateKeepAddress + 5));
  memcpy(&keepCall[1], &keepOffset, sizeof(keepOffset));
  patchCode("JadeEmpire!integrateKeep", integrateKeepAddress, integrateKeep, keepCall, sizeof(keepCall));
  installHook("JadeEmpire!forcesStep", forcesStepAddress, reinterpret_cast<void*>(&forcesStepDetour),
    reinterpret_cast<void**>(&originalForcesStep));
  installHook("JadeEmpire!integrateStep", integrateStepAddress, reinterpret_cast<void*>(&integrateStepDetour),
    reinterpret_cast<void**>(&originalIntegrateStep));
}
