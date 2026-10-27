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

typedef void (__thiscall* TextureUpdateFunction)(void* texture);
typedef void* (__thiscall* ScrollVectorFunction)(void* vector, float, float, float, float);

struct ProceduralType {
  const char* name;
  void* update;
  unsigned char prologue[8];
  TextureUpdateFunction original;
  unsigned long calls;
  unsigned long objects;
};

static const char* const toolName = "textureTimestep";
static const char* const textureTimestepVersion = "0.3.0";
static const char* const gameProgramName = "JadeEmpire.exe";
// A life texture regenerates every fifth rendered frame: 6 times a second at the game's own 30 frames per second.
static const LONGLONG lifeUpdatesPerSecond = 6;
static const size_t prunedObjectCount = 4096;
static const LONGLONG summaryMilliseconds = 60000;
static const size_t lifeIndex = 0;
// The game's rendered frame counter. Its frame time global measures the world's update, which stands still on the main
// menu, so the scroll measures real time between rendered frames itself.
static const volatile LONG* const gameFrameCounter = reinterpret_cast<const volatile LONG*>(0x0076EC5C);
static const float originalFramesPerSecond = 30.0f;
static const double longestFrameSeconds = 0.25;
// Builds the four-float offset (the mesh's two scroll speeds, twice) the scene walk at 0x60A0B0 adds to a scrolling
// mesh's texture coordinates once per frame; its only callers are that walk.
static void* const scrollVectorAddress = reinterpret_cast<void*>(0x005F5700);
static const unsigned char scrollVectorPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x10 };

// The Steam JadeEmpire.exe (link time 0x45B6777A), unpacked: each procedural texture type's update (method 1).
static ProceduralType proceduralTypes[] = {
  { "life", reinterpret_cast<void*>(0x0061A890), { 0x83, 0xEC, 0x3C, 0xA1, 0x5C, 0xEC, 0x76, 0x00 }, nullptr, 0, 0 },
  { "perlin", reinterpret_cast<void*>(0x0061B550), { 0x51, 0xA1, 0x5C, 0xEC, 0x76, 0x00, 0x55, 0x8B }, nullptr, 0, 0 },
  { "water", reinterpret_cast<void*>(0x0061B6C0), { 0xA1, 0x5C, 0xEC, 0x76, 0x00, 0x83, 0xEC, 0x60 }, nullptr, 0, 0 },
  { "wave", reinterpret_cast<void*>(0x0061BD70), { 0x83, 0xEC, 0x50, 0xA1, 0x5C, 0xEC, 0x76, 0x00 }, nullptr, 0, 0 },
  { "cycle", reinterpret_cast<void*>(0x0061AE20), { 0x51, 0xA1, 0x5C, 0xEC, 0x76, 0x00, 0x56, 0x8B }, nullptr, 0, 0 },
  { "arturo", reinterpret_cast<void*>(0x0061C1C0), { 0xD9, 0x05, 0xD0, 0xEC, 0x75, 0x00, 0x83, 0xEC }, nullptr, 0, 0 },
};
static const size_t proceduralTypeCount = sizeof(proceduralTypes) / sizeof(proceduralTypes[0]);

static bool isLoaded = false;
static bool isHooked = false;
static LONGLONG counterFrequency = 0;
static LONGLONG lifePeriod = 0;
static LONGLONG summaryStart = 0;
static unsigned long lifeHeld = 0;
static std::unordered_map<void*, LONGLONG> nextLifeUpdateOf;
static std::unordered_map<void*, size_t> typeOfTexture;
static ScrollVectorFunction originalScrollVector = nullptr;
static LONG scrollFrame = -1;
static LONGLONG scrollFrameStart = 0;
static float scrollScale = 1.0f;

static LONGLONG counterNow() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

static void countCall(size_t typeIndex, void* texture) {
  proceduralTypes[typeIndex].calls++;
  if (typeOfTexture.emplace(texture, typeIndex).second) {
    proceduralTypes[typeIndex].objects++;
  }
  const LONGLONG now = counterNow();
  if (summaryStart == 0) {
    summaryStart = now;
    return;
  }
  if ((now - summaryStart) * 1000 / counterFrequency < summaryMilliseconds) {
    return;
  }
  for (size_t index = 0; index < proceduralTypeCount; index++) {
    ProceduralType& type = proceduralTypes[index];
    if (type.calls != 0) {
      writeLogLine("INFO", "textureSummary type=%s updates=%lu textures=%lu", type.name, type.calls, type.objects);
    }
    type.calls = 0;
    type.objects = 0;
  }
  writeLogLine("INFO", "lifeSummary held=%lu", lifeHeld);
  lifeHeld = 0;
  typeOfTexture.clear();
  summaryStart = now;
}

// A life texture runs its update at most lifeUpdatesPerSecond times a second of real time; the update's own every
// fifth frame check then always passes above 30 frames per second.
static void __fastcall lifeUpdateDetour(void* texture, void* unusedEdx) {
  (void)unusedEdx;
  countCall(lifeIndex, texture);
  const LONGLONG now = counterNow();
  if (nextLifeUpdateOf.size() > prunedObjectCount) {
    for (auto entry = nextLifeUpdateOf.begin(); entry != nextLifeUpdateOf.end();) {
      entry = now - entry->second > counterFrequency ? nextLifeUpdateOf.erase(entry) : std::next(entry);
    }
  }
  LONGLONG& nextUpdate = nextLifeUpdateOf[texture];
  if (nextUpdate != 0 && now < nextUpdate) {
    lifeHeld++;
    return;
  }
  nextUpdate = nextUpdate == 0 || now - nextUpdate > lifePeriod ? now + lifePeriod : nextUpdate + lifePeriod;
  proceduralTypes[lifeIndex].original(texture);
}

#define countingDetour(index) \
  static void __fastcall countingDetour##index(void* texture, void* unusedEdx) { \
    (void)unusedEdx; \
    countCall(index, texture); \
    proceduralTypes[index].original(texture); \
  }
countingDetour(1)
countingDetour(2)
countingDetour(3)
countingDetour(4)
countingDetour(5)

// Scales the per-frame scroll by the frame's real time over the 1/30 s the game was built around, so the scroll per
// second is the original game's at any frame rate.
static void* __fastcall scrollVectorDetour(void* vector, void* unusedEdx, float first, float second, float third,
    float fourth) {
  (void)unusedEdx;
  const LONG frame = *gameFrameCounter;
  if (frame != scrollFrame) {
    const LONGLONG now = counterNow();
    double seconds = scrollFrameStart == 0 ? 1.0 / originalFramesPerSecond :
      static_cast<double>(now - scrollFrameStart) / counterFrequency;
    seconds = seconds > longestFrameSeconds ? longestFrameSeconds : seconds;
    scrollScale = static_cast<float>(seconds * originalFramesPerSecond);
    scrollFrame = frame;
    scrollFrameStart = now;
  }
  return originalScrollVector(vector, first * scrollScale, second * scrollScale, third * scrollScale,
    fourth * scrollScale);
}

static void* const detours[] = { reinterpret_cast<void*>(&lifeUpdateDetour), reinterpret_cast<void*>(&countingDetour1),
  reinterpret_cast<void*>(&countingDetour2), reinterpret_cast<void*>(&countingDetour3),
  reinterpret_cast<void*>(&countingDetour4), reinterpret_cast<void*>(&countingDetour5) };

// The code is still packed while the tools load; by the first message pump the game is running unpacked.
static void onPump() {
  if (isHooked) {
    return;
  }
  isHooked = true;
  for (size_t index = 0; index < proceduralTypeCount; index++) {
    ProceduralType& type = proceduralTypes[index];
    if (memcmp(type.update, type.prologue, sizeof(type.prologue)) != 0) {
      fatalError("The %s texture update at %p is not the code this tool was written for: this is a different JadeEmpire.exe",
        type.name, type.update);
    }
    char hookName[64];
    snprintf(hookName, sizeof(hookName), "JadeEmpire!%sTextureUpdate", type.name);
    installHook(hookName, type.update, detours[index], reinterpret_cast<void**>(&type.original));
  }
  if (memcmp(scrollVectorAddress, scrollVectorPrologue, sizeof(scrollVectorPrologue)) != 0) {
    fatalError("The scroll offset builder at %p is not the code this tool was written for: this is a different JadeEmpire.exe",
      scrollVectorAddress);
  }
  installHook("JadeEmpire!scrollVector", scrollVectorAddress, reinterpret_cast<void*>(&scrollVectorDetour),
    reinterpret_cast<void**>(&originalScrollVector));
}

extern "C" {

// The game's procedural textures (water, wave, cycle, perlin, arturo and life) update once per rendered frame; all but
// life advance by the frame's real time, while life regenerates every fifth frame, 6 times a second at the game's own
// 30 frames per second and faster above it. This holds each life texture to 6 updates a second of real time, and logs
// how often each type updates each minute. Scrolling meshes (the main menu's waterfall among them) move their texture
// coordinates a fixed step per frame; this scales that step by the frame's real time over 1/30 s. In the
// configuration program it does nothing. Every failure is fatal.
__declspec(dllexport) DWORD Load(const ToolHost* host) {
  if (isLoaded) {
    fatalError("Load was called twice");
  }
  isLoaded = true;

  setFatalErrorTitle(toolName);
  openSessionLog(host->toolDirectory, toolName, host->sessionStamp);
  writeLogLine("INFO", "sessionStart version=%s buildCommit=%s mainThreadID=%lu", textureTimestepVersion,
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
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  counterFrequency = frequency.QuadPart;
  lifePeriod = counterFrequency / lifeUpdatesPerSecond;
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
