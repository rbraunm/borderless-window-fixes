#include "gameClock.h"

#include "fatalError.h"
#include "hookRegistry.h"

#include <cstring>

typedef long long (__thiscall* EngineClockFunction)(void* clock);

// The Steam JadeEmpire.exe (link time 0x45B6777A), unpacked.
static const volatile LONG* const renderedFrameCounter = reinterpret_cast<const volatile LONG*>(0x0076EC5C);
// The world's frame time, which the game floors at 0.0001 s; it sits on the floor while the world is paused.
static const volatile float* const worldFrameSeconds = reinterpret_cast<const volatile float*>(0x0075ECD0);
static const float pausedWorldFrameSeconds = 0.00015f;
// The engine clock: in mode 1 with high resolution 1, it returns the performance counter over the clock's ticks per
// microsecond (a double at +0x18), in microseconds, computed on the x87 unit.
static void* const engineClockAddress = reinterpret_cast<void*>(0x00653190);
static const unsigned char engineClockPrologue[] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x83, 0x3E, 0x01 };
static const double longestFrameSeconds = 0.25;
static const double firstFrameSeconds = 1.0 / 30.0;

static LONGLONG counterFrequency = 0;
static LONG clockFrame = -1;
static LONGLONG clockFrameStart = 0;
static double frameSeconds = firstFrameSeconds;
static double gameTime = 0.0;
static EngineClockFunction originalEngineClock = nullptr;

LONGLONG counterNow() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

void initializeGameClock() {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  counterFrequency = frequency.QuadPart;
}

static void advanceToCurrentFrame() {
  const LONG frame = *renderedFrameCounter;
  if (frame == clockFrame) {
    return;
  }
  const LONGLONG now = counterNow();
  double realSeconds = clockFrameStart == 0 ? firstFrameSeconds :
    static_cast<double>(now - clockFrameStart) / static_cast<double>(counterFrequency);
  realSeconds = realSeconds > longestFrameSeconds ? longestFrameSeconds : realSeconds;
  const float world = *worldFrameSeconds;
  frameSeconds = world > pausedWorldFrameSeconds ? world : realSeconds;
  gameTime += frameSeconds;
  clockFrame = frame;
  clockFrameStart = now;
}

double gameFrameSeconds() {
  advanceToCurrentFrame();
  return frameSeconds;
}

double gameTimeNow() {
  advanceToCurrentFrame();
  return gameTime;
}

static long long __fastcall engineClockDetour(void* clock, void* unusedEdx) {
  (void)unusedEdx;
  const int* const modes = static_cast<const int*>(clock);
  if (modes[0] != 1 || modes[1] != 1) {
    return originalEngineClock(clock);
  }
  double ticksPerMicrosecond = 0.0;
  memcpy(&ticksPerMicrosecond, static_cast<const char*>(clock) + 0x18, sizeof(ticksPerMicrosecond));
  return static_cast<long long>(static_cast<double>(counterNow()) / ticksPerMicrosecond);
}

void installPreciseClock() {
  if (memcmp(engineClockAddress, engineClockPrologue, sizeof(engineClockPrologue)) != 0) {
    fatalError("The engine clock at %p is not the code this tool was written for: this is a different JadeEmpire.exe",
      engineClockAddress);
  }
  installHook("JadeEmpire!engineClock", engineClockAddress, reinterpret_cast<void*>(&engineClockDetour),
    reinterpret_cast<void**>(&originalEngineClock));
}
