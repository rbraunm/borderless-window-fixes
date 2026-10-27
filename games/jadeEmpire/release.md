# Jade Empire borderless window and timing fixes
Makes the Steam edition of Jade Empire: Special Edition start and run properly on today's Windows and monitors:

- **Starts from Steam** without "Failed to find Steam." when the game sits in a Steam library on a different drive than Steam itself.
- **Goes straight into the game**: Steam's Play skips the launcher, and the configuration utility, which only offered 4:3 resolutions and crashed after its hardware scan, is skipped too.
- **Borderless window at your desktop's resolution and refresh rate** in place of exclusive fullscreen: no display mode switch, and alt-tab leaves the game as an ordinary window that keeps rendering. The resolution menu offers only the desktop mode, and the settings file gets it, widescreen included, on every start.
- **Runs at your monitor's refresh rate** instead of the game's 30 frames per second cap, with cloth physics, wind, scrolling water and animated textures on game time, so they move at their designed speed at any frame rate and slow down with the game's own slow motion.
- **Fixes the world stuttering on a computer that has been on for days**: the game's clock lost precision the longer Windows had been running, until the world only updated about 15 times a second.
- **No flash of loading art** before the intro movies.

## Install
1. Extract the zip into the folder that holds `JadeEmpire.exe`. In Steam: right-click Jade Empire, Manage, Browse local files.
2. Play from Steam as usual. Steam still asks for administrator rights each time, because its install script marks the game's programs that way on every launch.

It adds `version.dll` and `msimg32.dll` beside `JadeEmpire.exe` and a `windowFixes` folder, and changes none of the game's files. Settings, such as the frame rate limit (`d3d9Borderless.cfg`) and the physics rate (`timeFixes.cfg`), are written into `windowFixes` the first time the game starts.

## Remove
Delete `version.dll`, `msimg32.dll` and the `windowFixes` folder.
