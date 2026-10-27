#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"
#include "settingsFile.h"
#include "toolHost.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <unordered_map>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

typedef void (__thiscall* PhysicsStepFunction)(void* object);

static const char* const toolName = "physicsTimestep";
static const char* const physicsTimestepVersion = "0.3.0";
static const char* const configurationFileName = "physicsTimestep.cfg";
static const char* const gameProgramName = "JadeEmpire.exe";
static const double originalStepsPerSecond = 30.0;
// One frame catches up at most this much time, as the game caps its own frame time; an object further behind starts its
// steps over.
static const double longestCatchUpSeconds = 0.25;
static const size_t prunedObjectCount = 4096;
static const LONGLONG summaryMilliseconds = 60000;

// The Steam JadeEmpire.exe (link time 0x45B6777A), unpacked. Per physics object (cloth among them), once per frame and
// back to back: the forces step, which steps by max(frame time, 1/30 s), and the integration, which steps by 1/30 s
// times the object's time scale and keeps 1 - damping of each point's motion per step.
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

static bool isLoaded = false;
static bool isHooked = false;
static DWORD gameMainThreadID = 0;
static PhysicsStepFunction originalForcesStep = nullptr;
static PhysicsStepFunction originalIntegrateStep = nullptr;
static LONGLONG counterFrequency = 0;
static unsigned long stepsPerSecond = 0;
static LONGLONG fixedStepTicks = 0;
static LONGLONG catchUpTicks = 0;
// What the patched code reads: the step in seconds, and the power the kept share of motion is raised to.
static float stepSeconds = 0.0f;
static double keepExponent = 1.0;
static std::unordered_map<void*, LONGLONG> nextStepOf;
static LONGLONG summaryStart = 0;
static unsigned long stepsRun = 0;
static unsigned long framesWithoutStep = 0;

static LONGLONG counterNow() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

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

static void logSummary(LONGLONG now) {
  if (summaryStart == 0) {
    summaryStart = now;
    return;
  }
  if ((now - summaryStart) * 1000 / counterFrequency < summaryMilliseconds) {
    return;
  }
  writeLogLine("INFO", "stepSummary seconds=%lld steps=%lu skippedObjectFrames=%lu objects=%zu", (now - summaryStart) / counterFrequency,
    stepsRun, framesWithoutStep, nextStepOf.size());
  summaryStart = now;
  stepsRun = 0;
  framesWithoutStep = 0;
}

static void runStep(void* object, double seconds) {
  stepSeconds = static_cast<float>(seconds);
  keepExponent = seconds * originalStepsPerSecond;
  originalForcesStep(object);
  originalIntegrateStep(object);
  stepsRun++;
}

// Takes over the pair: the game calls the forces step once per frame, this runs as many whole fixed steps (forces then
// integration) as the object's real time calls for, none to several, and the game's own integration call that follows
// does nothing. The steps per second never depend on the frame rate.
static void __fastcall forcesStepDetour(void* object, void* unusedEdx) {
  (void)unusedEdx;
  if (GetCurrentThreadId() != gameMainThreadID) {
    fatalError("The forces step ran on thread %lu, not the game's main thread %lu", GetCurrentThreadId(), gameMainThreadID);
  }
  const LONGLONG now = counterNow();
  logSummary(now);
  if (nextStepOf.size() > prunedObjectCount) {
    for (auto entry = nextStepOf.begin(); entry != nextStepOf.end();) {
      entry = now - entry->second > counterFrequency ? nextStepOf.erase(entry) : std::next(entry);
    }
  }
  LONGLONG& nextStep = nextStepOf[object];
  if (nextStep == 0 || now - nextStep > catchUpTicks) {
    nextStep = now;
  }
  if (now < nextStep) {
    framesWithoutStep++;
    return;
  }
  while (nextStep <= now) {
    runStep(object, 1.0 / static_cast<double>(stepsPerSecond));
    nextStep += fixedStepTicks;
  }
}

static void __fastcall integrateStepDetour(void* object, void* unusedEdx) {
  (void)object;
  (void)unusedEdx;
}

static void* addressOfStep() {
  return &stepSeconds;
}

// The code is still packed while the tools load; by the first message pump the game is running unpacked.
static void onPump() {
  if (isHooked) {
    return;
  }
  isHooked = true;
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

extern "C" {

// The game simulates its physics objects (cloth among them) one 1/30 s step per frame, right at its own 30 frames per
// second cap and faster the higher the frame rate. This steps them by real time instead, stepsPerSecond fixed steps a
// second (30 is the original game) whatever the frame rate, and scales the damping each step applies to keep the
// original's per second. In the configuration program it does nothing. Every failure is fatal.
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
  const SettingDefinition definitions[] = {
    { "stepsPerSecond", SettingKind::positiveInteger, &stepsPerSecond, false },
  };
  loadSettingsFile(host->toolDirectory, configurationFileName, definitions, sizeof(definitions) / sizeof(definitions[0]));
  writeLogLine("INFO", "configurationLoaded stepsPerSecond=%lu", stepsPerSecond);
  gameMainThreadID = host->mainThreadID;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  counterFrequency = frequency.QuadPart;
  fixedStepTicks = counterFrequency / static_cast<LONGLONG>(stepsPerSecond);
  catchUpTicks = static_cast<LONGLONG>(longestCatchUpSeconds * static_cast<double>(counterFrequency));
  initializeHookRegistry();
  host->addPumpCallback(&onPump);
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
