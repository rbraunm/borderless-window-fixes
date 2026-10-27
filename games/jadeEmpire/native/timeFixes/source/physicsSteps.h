#pragma once

#include <windows.h>

// The game simulates its physics objects (cloth among them) one 1/30 s step per rendered frame. This steps them by
// game time instead, stepsPerSecond fixed steps a second of it whatever the frame rate, and scales the damping each
// step applies to keep the original's per second. Call once the game's code is unpacked.
void installPhysicsSteps(unsigned long stepsPerSecond, DWORD mainThreadID);
