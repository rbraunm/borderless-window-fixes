#include <windows.h>

#include "animationTiming.h"

#include "fatalError.h"
#include "gameClock.h"
#include "hookRegistry.h"

#include <cstring>
#include <unordered_map>

typedef void (__thiscall* ObjectFunction)(void* object);
typedef void* (__thiscall* ScrollVectorFunction)(void* vector, float, float, float, float);

static const double originalFramesPerSecond = 30.0;
static const double lifeUpdatesPerSecond = 6.0;
static const double staleTextureSeconds = 1.0;
static const size_t prunedTextureCount = 4096;

// The Steam JadeEmpire.exe (link time 0x45B6777A), unpacked.
// A life procedural texture's update (method 1 of its type).
static void* const lifeUpdateAddress = reinterpret_cast<void*>(0x0061A890);
static const unsigned char lifeUpdatePrologue[] = { 0x83, 0xEC, 0x3C, 0xA1, 0x5C, 0xEC, 0x76, 0x00 };
// Builds the four-float offset (a mesh's two scroll speeds, twice) the scene walk at 0x60A0B0 adds to a scrolling
// mesh's texture coordinates once per frame; its only callers are that walk.
static void* const scrollVectorAddress = reinterpret_cast<void*>(0x005F5700);
static const unsigned char scrollVectorPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x10 };
// The area's wind tick, and its fld of the world's frame time for the wind's advance.
static void* const windTickAddress = reinterpret_cast<void*>(0x005E7330);
static const unsigned char windTickPrologue[] = { 0x83, 0x81, 0x04, 0x04, 0x00, 0x00, 0x01 };
static unsigned char* const windTimeLoadAddress = reinterpret_cast<unsigned char*>(0x005E734B);
static const unsigned char windTimeLoad[] = { 0xD9, 0x05, 0xD0, 0xEC, 0x75, 0x00 };

static ObjectFunction originalLifeUpdate = nullptr;
static ScrollVectorFunction originalScrollVector = nullptr;
static ObjectFunction originalWindTick = nullptr;
static std::unordered_map<void*, double> nextLifeUpdateOf;
// What the patched wind tick reads in place of the world's frame time.
static float windFrameSeconds = 0.0f;

// A life texture runs its update at most lifeUpdatesPerSecond times a second of game time; the update's own every fifth
// frame check then always passes above 30 frames per second.
static void __fastcall lifeUpdateDetour(void* texture, void* unusedEdx) {
  (void)unusedEdx;
  const double now = gameTimeNow();
  if (nextLifeUpdateOf.size() > prunedTextureCount) {
    for (auto entry = nextLifeUpdateOf.begin(); entry != nextLifeUpdateOf.end();) {
      entry = now - entry->second > staleTextureSeconds ? nextLifeUpdateOf.erase(entry) : std::next(entry);
    }
  }
  auto found = nextLifeUpdateOf.find(texture);
  if (found != nextLifeUpdateOf.end() && now < found->second) {
    return;
  }
  const double period = 1.0 / lifeUpdatesPerSecond;
  nextLifeUpdateOf[texture] = found == nextLifeUpdateOf.end() || now - found->second > period ? now + period :
    found->second + period;
  originalLifeUpdate(texture);
}

static void* __fastcall scrollVectorDetour(void* vector, void* unusedEdx, float first, float second, float third,
    float fourth) {
  (void)unusedEdx;
  const float scale = static_cast<float>(gameFrameSeconds() * originalFramesPerSecond);
  return originalScrollVector(vector, first * scale, second * scale, third * scale, fourth * scale);
}

static void __fastcall windTickDetour(void* area, void* unusedEdx) {
  (void)unusedEdx;
  windFrameSeconds = static_cast<float>(gameFrameSeconds());
  originalWindTick(area);
}

static void hookChecked(const char* name, void* address, const unsigned char* prologue, size_t prologueSize, void* detour,
    void** original) {
  if (memcmp(address, prologue, prologueSize) != 0) {
    fatalError("%s at %p is not the code this tool was written for: this is a different JadeEmpire.exe", name, address);
  }
  installHook(name, address, detour, original);
}

static void* addressOfWindFrameSeconds() {
  return &windFrameSeconds;
}

void installAnimationTiming() {
  hookChecked("JadeEmpire!lifeTextureUpdate", lifeUpdateAddress, lifeUpdatePrologue, sizeof(lifeUpdatePrologue),
    reinterpret_cast<void*>(&lifeUpdateDetour), reinterpret_cast<void**>(&originalLifeUpdate));
  hookChecked("JadeEmpire!scrollVector", scrollVectorAddress, scrollVectorPrologue, sizeof(scrollVectorPrologue),
    reinterpret_cast<void*>(&scrollVectorDetour), reinterpret_cast<void**>(&originalScrollVector));
  const DWORD windSecondsAddress = static_cast<DWORD>(reinterpret_cast<uintptr_t>(addressOfWindFrameSeconds()));
  unsigned char loadWindSeconds[sizeof(windTimeLoad)] = { 0xD9, 0x05 };
  memcpy(&loadWindSeconds[2], &windSecondsAddress, sizeof(windSecondsAddress));
  patchCode("JadeEmpire!windTimeLoad", windTimeLoadAddress, windTimeLoad, loadWindSeconds, sizeof(loadWindSeconds));
  hookChecked("JadeEmpire!windTick", windTickAddress, windTickPrologue, sizeof(windTickPrologue),
    reinterpret_cast<void*>(&windTickDetour), reinterpret_cast<void**>(&originalWindTick));
}
