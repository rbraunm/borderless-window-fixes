#pragma once

// Samples every active monitor four times a second on a thread of its own and logs each change of its desktop mode,
// the refresh rate its link runs at, and its advanced color (HDR) state, so a switch made by the driver or Windows
// rather than the game still shows up.
void startMonitorWatch();
