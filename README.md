# borderless-window-fixes
Fixes for 64-bit Windows games that misbehave in borderless windowed mode on large and high-DPI monitors: windows that flash, stop rendering after losing focus, or keep hold of the mouse and keyboard. Each fix is a standalone DLL loaded into the game by a `version.dll` proxy, with its settings and logs in a `windowFixes` folder beside the game's executable. The game's own files are never modified. No installer, no registry.

Started from the AmmolitePreservation client toolsets (clearcoat, scarab, ddo-client-tools); this repository depends on none of them.

| Folder | What it does |
|---|---|
| `native/fixLoader/` | Builds `version.dll`, a proxy for the system copy. Windows picks it up from the executable's folder when the game or one of its DLLs imports `version.dll`. At the game's entry point, on the main thread and before any game code runs, it brings `windowFixes.txt` up to the release's tools (see Settings files), then loads each DLL named in `windowFixes.txt` from a shadow copy and calls its `Load` export. While the game runs, it reloads the tools that export `Unload` on request through `\\.\pipe\fixLoader-<process ID>` (`tools`, `reload <dll>`). |
| `native/stabilityGuard/` | Crash and freeze evidence: a crash the game may survive gets a report (registers, unwound frames, stack candidates, modules) and a small memory dump; a crash that ends the game, or a window that stops responding for `freezeSeconds`, gets a dump of all the game's memory; and every way the process ends itself is logged with its exit code and stack. Dumps sit in `logs\stabilityGuard`, the oldest removed past `maxDumpGigabytes`. Also makes quitting always work: closing the window arms a timer that ends a game stuck on its way out, and Ctrl+Alt+F4 ends the game in front at once. Settings in `stabilityGuard.cfg`. |
| `native/displayGuard/` | Reports what the game does with its display, without changing it: every monitor and its DPI, the process's DPI awareness, every top-level window the main thread creates and each later change of its style or placement, every display mode change, and which graphics APIs the game loads at runtime. Around focus: activation, focus, size and close messages; mouse confinement, capture, visibility, cursor warps, raw input registrations and Windows hooks, each with the module that asked; SDL2's grab, relative mouse, minimize, fullscreen and hint calls when the game uses SDL2; and OpenGL frame pacing (stalls, frames per second for 10 seconds after each activation change, a summary each minute). Every change of any monitor's desktop mode, link refresh rate or HDR state, sampled four times a second, and every display mode change request in the process at the system call, whoever made it. What to fix comes from these logs. |
| `native/borderlessFit/` | Sizes the game's borderless window when it asks for exactly its monitor's rectangle (popup style, main thread, at creation and on each resize): `heightAdjustment` is taller (one pixel past the bottom edge), shorter (one pixel short), or none. A window that matches its monitor exactly is taken over by some drivers as if it were fullscreen. |
| `native/shared/` | Source every tool compiles in: session log, fatal error, hook registry, command pipe, settings files, and vendored MinHook |
| `games/<game>/` | One game: `game.cfg` (its executable, relative to its install folder), `windowFixes.txt` (the tools its loader ships), and `native/<tool>/` for fixes only that game needs. |
| `python/windowFixes/` | A game from outside: launches it without taking the foreground, talks to the loader's pipe, reads the tools' session logs, and captures the game's window. |

## Games
| Game | Folder | Status |
|---|---|---|
| SOMA (Steam 282140, `Soma.exe` 1.110, HPL3 on SDL2 and OpenGL) | `games/soma` | Fixed by borderlessFit (taller). The NVIDIA driver took the monitor over whenever SOMA's OpenGL window exactly covered it and was in front, switching an HDR monitor to SDR and back on each focus change, which blanked it for 2 to 3 seconds; the driver's OpenGL present method made no difference. Open: no sound in the background, since the game sleeps while out of focus (`SleepWhenOutOfFocus`). |

## Game folder layout
```
<install folder>\
  <executable>            the game's executable, unmodified (game.cfg names it)
  version.dll             fixLoader; must sit beside the executable
  windowFixes\
    windowFixes.txt       the tools to load, in order
    <tool>.dll, <tool>.cfg
    loaded\               shadow copies the game runs from, so a build can replace any tool
    logs\<tool>\          <tool>_<date>_<time>_pid<process ID>.log, kept 90 days; stabilityGuard's .dmp files are kept by total size instead
```

## Build, test and install
On Windows with Visual Studio 2022 and its C++ x64 build tools (MSVC, ml64, CMake, Ninja). The Python client runs in the repository's virtual environment, made once with Python 3.14:

```powershell
python -m venv .venv
.venv\Scripts\python.exe -m pip install -e .
```

Each build is for one game, since the loader carries that game's tool list:

```powershell
.\build.ps1 -game soma -test -gameFolder "S:\Games\Steam\steamapps\common\SOMA"
```

Run it as `powershell -NoProfile -File build.ps1 ...` when calling from another PowerShell, which otherwise turns native stderr into errors. The build lands in `build\<game>\bin`. `-gameFolder` lays it out in that install folder as a release does (`layout.ps1`): `version.dll` beside the executable and the rest in `windowFixes`. It copies no settings files: the tools write and update their own (see Settings files). Removing `version.dll` from the install folder turns every fix off.

### Settings files
Each tool carries its settings files, embedded at build time, the one copy of them: each tool's `.cfg` from its folder, and the game's `windowFixes.txt` in fixLoader. A file missing from `windowFixes` is written with the shipped defaults when the game starts, so deleting one starts it over. An existing file is used as it is, and one from an earlier release is brought up to date by key: a setting it lacks is appended with its default and the comment above it in the shipped file (`settingAdded` in the tool's log), and a key the tool does not read, such as one a newer release dropped, or a line that is not key=value, is removed from the file and logged with its text (`settingRemoved`). `windowFixes.txt` is kept the same way: a tool a newer release adds is appended with its shipped state (`toolAdded`), and a line naming a tool the release does not ship and the folder does not hold is removed (`toolRemoved`).

## Adding a game
1. `games/<game>/game.cfg` with `executable=<path relative to the install folder>`; the executable must be 64-bit, and it or one of the DLLs it imports must import `version.dll`.
2. `games/<game>/windowFixes.txt` listing the shared tools it uses and its own.
3. Its own fixes, if any, in `games/<game>/native/<tool>/source/*.cpp` with an optional `<tool>.cfg`.

## Iterating
`windowFixes.session` keeps one game open across commands. It takes the game from `--game`, the install folder from `--gameFolder` or the `windowFixesGameFolder` environment variable, and records the launched process in `windowFixes\session.txt`:

```powershell
$env:windowFixesGameFolder = "S:\Games\Steam\steamapps\common\SOMA"
.venv\Scripts\python.exe -m windowFixes.session --game soma launch
.venv\Scripts\python.exe -m windowFixes.session --game soma log displayGuard
.venv\Scripts\python.exe -m windowFixes.session --game soma reload <tool>.dll
.venv\Scripts\python.exe -m windowFixes.session --game soma capture shot.png
.venv\Scripts\python.exe -m windowFixes.session --game soma close
```

`launch` passes its extra arguments to the game.
