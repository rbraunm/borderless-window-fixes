# SOMA borderless window fix
Stops SOMA's borderless window from blanking the screen for a few seconds each time the game gains or loses focus. On an HDR monitor with an NVIDIA card, the driver treats a window that exactly covers the monitor as fullscreen and switches the monitor out of HDR and back; this release makes SOMA's window one pixel taller than the monitor, which is off screen.

## Install
1. Extract the zip into the folder that holds `Soma.exe`. In Steam: right-click SOMA, Manage, Browse local files.
2. Play with SOMA's display mode set to borderless.

It adds `version.dll` beside `Soma.exe` and a `windowFixes` folder, and changes none of the game's files.

## Remove
Delete `version.dll` and the `windowFixes` folder.
