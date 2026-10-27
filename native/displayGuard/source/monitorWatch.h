#pragma once

// Samples every attached monitor's current mode (size, bits, refresh rate) four times a second on a thread of its
// own and logs each change, so a mode switch made by the driver or Windows rather than the game still shows up.
void startMonitorWatch();
