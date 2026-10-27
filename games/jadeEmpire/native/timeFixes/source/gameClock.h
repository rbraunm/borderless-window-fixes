#pragma once

#include <windows.h>

// The time the game's animation runs on. While the world runs, a rendered frame lasts the world's own frame time,
// which already carries the game's slow motion; while it is paused (the main menu), it lasts the real time since the
// last rendered frame. Read on the game's main thread.
void initializeGameClock();
double gameFrameSeconds();
double gameTimeNow();

LONGLONG counterNow();

// Replaces the engine's clock with the same reading in full double precision: Direct3D 9 leaves the x87 unit in
// single precision, which rounds the game's microsecond clock to steps that grow with the machine's uptime.
void installPreciseClock();
