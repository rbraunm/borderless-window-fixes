#include "fatalError.h"
#include "hookRegistry.h"
#include "sessionLog.h"
#include "settingsFile.h"
#include "toolHost.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#ifndef windowFixesBuildCommit
#error "windowFixesBuildCommit must be defined by the build (see build.ps1)"
#endif

typedef void (__thiscall* PhysicsStepFunction)(void* object);

static const char* const toolName = "physicsTimestep";
static const char* const physicsTimestepVersion = "0.2.0";
static const char* const configurationFileName = "physicsTimestep.cfg";
static const char* const frameLimitFileName = "d3d9Borderless.cfg";
static const char* const gameProgramName = "JadeEmpire.exe";
static const double originalStepsPerSecond = 30.0;
// One frame can run this many steps to catch up; an object further behind starts its steps over.
static const int catchUpStepLimit = 4;
// A step taken from a frame's real time (stepsPerSecond=0) stays within these bounds, as the game's own frame time does.
static const double shortestStep = 0.001;
static const double longestStep = 0.25;
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
// 0: one step per frame from the frame's real time; otherwise fixed steps at this rate.
static double stepsPerSecond = 0.0;
static LONGLONG fixedStepTicks = 0;
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

// Takes over the pair: the game calls the forces step, this runs as many whole steps (forces then integration) as
// the object's real time calls for, and the game's own integration call that follows does nothing.
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
  if (stepsPerSecond == 0.0) {
    double seconds = nextStep == 0 ? 1.0 / originalStepsPerSecond : static_cast<double>(now - nextStep) / counterFrequency;
    seconds = seconds < shortestStep ? shortestStep : seconds > longestStep ? longestStep : seconds;
    nextStep = now;
    runStep(object, seconds);
    return;
  }
  if (nextStep == 0 || now - nextStep > catchUpStepLimit * fixedStepTicks) {
    nextStep = now;
  }
  if (now < nextStep) {
    framesWithoutStep++;
    return;
  }
  while (nextStep <= now) {
    runStep(object, 1.0 / stepsPerSecond);
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

// frameRate: the frame rate d3d9Borderless holds the game to, auto being the primary monitor's refresh rate (the game
// draws on adapter 0).
static double frameRateLimit(const std::string& toolDirectory) {
  const std::string path = toolDirectory + "\\" + frameLimitFileName;
  FILE* file = fopen(path.c_str(), "r");
  if (file == nullptr) {
    fatalError("%s: stepsPerSecond=frameRate needs %s, which is missing", configurationFileName, path.c_str());
  }
  char line[256];
  std::string limit;
  while (fgets(line, sizeof(line), file) != nullptr) {
    if (strncmp(line, "frameRateLimit=", 15) == 0) {
      limit = line + 15;
      limit.erase(limit.find_last_not_of(" \t\r\n") + 1);
    }
  }
  fclose(file);
  if (limit == "auto") {
    DEVMODEA desktop = {};
    desktop.dmSize = sizeof(desktop);
    if (!EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &desktop)) {
      fatalError("EnumDisplaySettings failed for the primary monitor");
    }
    return desktop.dmDisplayFrequency;
  }
  const double rate = strtod(limit.c_str(), nullptr);
  if (limit.empty() || limit.find_first_not_of("0123456789") != std::string::npos || rate == 0.0) {
    fatalError("%s: stepsPerSecond=frameRate needs a frame rate limit in %s, which has frameRateLimit=%s",
      configurationFileName, frameLimitFileName, limit.c_str());
  }
  return rate;
}

extern "C" {

// The game simulates its physics objects (cloth among them) one 1/30 s step per frame, right at its own 30 frames per
// second cap and faster the higher the frame rate. This steps them by real time instead: stepsPerSecond fixed steps a
// second (30 is the original game; frameRate matches d3d9Borderless's frame rate limit), or one step per frame from
// the frame's own time with 0. The damping each step applies is scaled to keep the original's per second. In the
// configuration program it does nothing. Every failure is fatal.
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
  std::string stepsText;
  const SettingDefinition definitions[] = {
    { "stepsPerSecond", SettingKind::text, &stepsText, false },
  };
  loadSettingsFile(host->toolDirectory, configurationFileName, definitions, sizeof(definitions) / sizeof(definitions[0]));
  if (stepsText == "frameRate") {
    stepsPerSecond = frameRateLimit(host->toolDirectory);
  } else if (stepsText.find_first_not_of("0123456789") == std::string::npos && stepsText.size() <= 4) {
    stepsPerSecond = strtod(stepsText.c_str(), nullptr);
  } else {
    fatalError("%s: stepsPerSecond must be frameRate, 0 or a number of steps per second, not %s", configurationFileName,
      stepsText.c_str());
  }
  writeLogLine("INFO", "configurationLoaded stepsPerSecond=%s chosen=%.0f", stepsText.c_str(), stepsPerSecond);
  gameMainThreadID = host->mainThreadID;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  counterFrequency = frequency.QuadPart;
  fixedStepTicks = stepsPerSecond != 0.0 ? static_cast<LONGLONG>(counterFrequency / stepsPerSecond) : 0;
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
