# borderless-window-fixes
Fixes for Windows games, 64-bit and 32-bit, that misbehave in borderless windowed mode on large and high-DPI monitors or on today's Windows: windows that flash, stop rendering after losing focus, keep hold of the mouse and keyboard, or animation and physics tied to the frame rate. Each fix is a standalone DLL loaded into the game by a `version.dll` proxy, with its settings and logs in a `windowFixes` folder beside the game's executable. The game's own files are never modified. No installer, no registry.

Started from the AmmolitePreservation client toolsets (clearcoat, scarab, ddo-client-tools); this repository depends on none of them.

## Download and install
Each fixed game has its own [release](https://github.com/rbraunm/borderless-window-fixes/releases): a zip to extract into the folder that holds the game's executable. The release notes say what it fixes and anything the game needs set. To remove a fix, delete `version.dll` (and `msimg32.dll`, for a game whose release has one) and the `windowFixes` folder from the game's folder. Each tool writes its settings file into `windowFixes` the first time the game starts.

| Folder | What it does |
|---|---|
| `native/fixLoader/` | Builds `version.dll`, a proxy for the system copy. Windows picks it up from the executable's folder when the game or one of its DLLs imports `version.dll`. It acts only in the game's executable and its companions (game.cfg); in any other program in the folder it only passes calls on. On the main thread and before any game code runs, it brings `windowFixes.txt` up to the release's tools (see Settings files), then loads each DLL named there from a shadow copy and calls its `Load` export: at the game's entry point when Windows loads it at startup, or, when a packed executable loads it while unpacking itself, at that thread's first `LoadLibrary`, `GetProcAddress` or C runtime startup call outside the loader lock (`startMode` in its log). While the game runs, it reloads the tools that export `Unload` on request through `\\.\pipe\fixLoader-<process ID>` (`tools`, `reload <dll>`). |
| `native/launcherSkip/` | For a game whose Steam entry starts a launcher (game.cfg `launcher=`): built as `msimg32.dll`, which the launcher imports, it starts the game at the launcher's entry point and ends the launcher. In any other program it passes calls on to the system copy. x86 only so far. |
| `native/stabilityGuard/` | Diagnostic, not in releases. Crash and freeze evidence: a crash the game may survive gets a report (registers, unwound frames, stack candidates, modules) and a small memory dump; a crash that ends the game, or a window that stops responding for `freezeSeconds`, gets a dump of all the game's memory; and every way the process ends itself is logged with its exit code and stack. Dumps sit in `logs\stabilityGuard`, the oldest removed past `maxDumpGigabytes`. Also makes quitting always work: closing the window arms a timer that ends a game stuck on its way out, and Ctrl+Alt+F4 ends the game in front at once. Settings in `stabilityGuard.cfg`. |
| `native/displayGuard/` | Diagnostic, not in releases. Reports what the game does with its display, without changing it: every monitor and its DPI, the process's DPI awareness, every top-level window the main thread creates and each later change of its style or placement, every display mode change, and which graphics APIs the game loads at runtime. Around focus: activation, focus, size and close messages; mouse confinement, capture, visibility, cursor warps, raw input registrations and Windows hooks, each with the module that asked; SDL2's grab, relative mouse, minimize, fullscreen and hint calls when the game uses SDL2; and frame pacing at OpenGL's `SwapBuffers` and Direct3D 9's `Present` (stalls, frames per second for 10 seconds after each activation change, a summary each minute). Every change of any monitor's desktop mode, link refresh rate or HDR state, sampled four times a second, and every display mode change request in the process at the system call, whoever made it. What to fix comes from these logs. |
| `native/d3d9Borderless/` | Turns a Direct3D 9 game's exclusive fullscreen into a borderless window at the desktop's own mode: the device is created and reset windowed, the device window becomes a popup over its monitor (the game still decides when it is shown), and the game's resolution list offers only the desktop mode. `frameRateLimit` holds the frame rate at `IDirect3DDevice9::Present`: `auto` (the monitor's refresh rate), `0` (none) or a number. |
| `native/borderlessFit/` | Sizes the game's borderless window when it asks for exactly its monitor's rectangle (popup style, main thread, at creation and on each resize): `heightAdjustment` is taller (one pixel past the bottom edge), shorter (one pixel short), or none. A window that matches its monitor exactly is taken over by some drivers as if it were fullscreen. |
| `native/shared/` | Source every tool compiles in: session log, fatal error, hook registry and code patches, command pipe, settings files, and vendored MinHook |
| `games/<game>/` | One game: `game.cfg`, `windowFixes.txt` (the tools its release ships), `release.md` (its release notes, starting with a `#` title), `native/<tool>/` for fixes only that game needs, and `<tool>.cfg` for any tool whose shipped settings differ for this game. |
| `python/windowFixes/` | A game from outside: launches it without taking the foreground, talks to the loader's pipe, reads the tools' session logs, and captures the game's window. |

## Games
| Game | Folder | Status |
|---|---|---|
| Jade Empire: Special Edition (Steam 7110, `JadeEmpire.exe` behind Steam's DRM wrapper, x86, Direct3D 9) | `games/jadeEmpire` | Fixed in the main menu and character creation; gameplay and combat not yet played. steamLocator loads Steam's own `steam.dll` (the game looks for it only beside itself); configBypass and launcherSkip skip the configuration utility and the launcher; renderSettings writes the desktop mode, widescreen and `ClampFPS=0` into `JadeEmpire.ini`; d3d9Borderless runs it borderless at the refresh rate; introHold presents nothing before the first intro movie; timeFixes computes the engine clock in double precision (Direct3D 9's single precision x87 rounded it to steps of 65.5 ms after 6.4 days of uptime, so the world updated about 15 times a second) and puts physics (120 fixed steps a second), life textures, mesh scrolling and wind on game time. Each Steam launch still asks for administrator rights: Steam's install script sets that on every launch. |
| SOMA (Steam 282140, `Soma.exe` 1.110, HPL3 on SDL2 and OpenGL) | `games/soma` | Released. Fixed by borderlessFit (taller). The NVIDIA driver took the monitor over whenever SOMA's OpenGL window exactly covered it and was in front, switching an HDR monitor to SDR and back on each focus change, which blanked it for 2 to 3 seconds; the driver's OpenGL present method made no difference. Open: no sound in the background, since the game sleeps while out of focus (`SleepWhenOutOfFocus`). |

## Game folder layout
```
<install folder>\
  <executable>            the game's executable, unmodified (game.cfg names it)
  version.dll             fixLoader; must sit beside the executable
  msimg32.dll             launcherSkip, for a game with a launcher
  windowFixes\
    windowFixes.txt       the tools to load, in order
    <tool>.dll, <tool>.cfg
    loaded\               shadow copies the game runs from, so a build can replace any tool
    logs\<tool>\          <tool>_<date>_<time>_pid<process ID>.log, kept 90 days; stabilityGuard's .dmp files are kept by total size instead
```

## Build, test and install
On Windows with Visual Studio 2022 and its C++ x86 and x64 build tools (MSVC, ml and ml64, CMake, Ninja). The Python client runs in the repository's virtual environment, made once with Python 3.14:

```powershell
python -m venv .venv
.venv\Scripts\python.exe -m pip install -e .
```

Each build is for one game and its architecture (game.cfg), since the loader carries that game's tool list:

```powershell
.\build.ps1 -game soma -test -diagnostics -gameFolder "<SOMA install folder>"
```

Run it as `powershell -NoProfile -File build.ps1 ...` when calling from another PowerShell, which otherwise turns native stderr into errors. The build lands in `build\<game>\bin`. `-gameFolder` lays it out in that install folder as a release does (`layout.ps1`): `version.dll` beside the executable and the rest in `windowFixes`. It copies no settings files: the tools write and update their own (see Settings files). `-diagnostics` also installs stabilityGuard and displayGuard, built for both architectures, and lists them in `windowFixes.txt`; the loader keeps a listed tool the release does not ship as long as its DLL is in the folder. Removing `version.dll` from the install folder turns every fix off.

`.\package.ps1 -game soma -outputPath <zip>` packs a built game's release: the same layout, without the diagnostic tools.

### Releases
A game is released only when its release files change. `release.ps1` builds every game under `games` with `-buildCommit unversioned`, so the DLLs depend only on the code; builds are reproducible (`/Brepro`). It then fingerprints the files the game's zip ships, and compares that with the fingerprint in the notes of the game's newest release. A game whose fingerprint differs is built again with the commit stamped in, packed, and, with `-publish`, released as `<game>-<yyyy.MM.dd>-<commit>`, with its `release.md` as title and notes. A change to a tool the game does not ship, or to anything outside the code, leaves its release alone. A game with no `release.md` yet is built but never released.

`.github/workflows/release.yml` runs `release.ps1 -test` on each pull request to `main`, which reports the games that would be released, and `release.ps1 -test -publish` on each push to `main`.

### game.cfg
- `executable=` the game's executable, relative to its install folder; `version.dll` and `windowFixes` go beside it.
- `architecture=` `x64` or `x86`, which picks the build.
- `companion=` (any number) another program beside the executable that also gets the tools, such as one the launcher runs before the game.
- `launcher=` (at most one) the launcher the game's Steam entry starts, which launcherSkip replaces with the game.

### Settings files
Each tool carries its settings files, embedded at build time, the one copy of them: each tool's `.cfg` from its folder (or the game's own copy in `games/<game>/`, which replaces it for that game), and the game's `windowFixes.txt` in fixLoader. A file missing from `windowFixes` is written with the shipped defaults when the game starts, so deleting one starts it over. An existing file is used as it is, and one from an earlier release is brought up to date by key: a setting it lacks is appended with its default and the comment above it in the shipped file (`settingAdded` in the tool's log), and a key the tool does not read, such as one a newer release dropped, or a line that is not key=value, is removed from the file and logged with its text (`settingRemoved`). `windowFixes.txt` is kept the same way: a tool a newer release adds is appended with its shipped state (`toolAdded`), and a line naming a tool the release does not ship and the folder does not hold is removed (`toolRemoved`).

## Adding a game
Report first, then fix: the diagnostic tools record what the game does, and a fix is written against what their logs show.

1. `games/<game>/game.cfg` (see game.cfg). The executable, or a DLL it loads, must load `version.dll`, at startup or while it unpacks itself.
2. `games/<game>/windowFixes.txt` and `games/<game>/release.md`.
3. Build with `-diagnostics -gameFolder <install folder>`, reproduce the problem, and read `windowFixes\logs\displayGuard` and `stabilityGuard` (see Iterating).
4. Write the fix as a tool:
   - one other games could use goes in `native/<tool>/` and gets an `addToolLibrary` line in `CMakeLists.txt`, like borderlessFit;
   - one only this game needs goes in `games/<game>/native/<tool>/`, which the build picks up on its own.

   A tool is a DLL exporting `Load(const ToolHost*)`, which fixLoader calls on the game's main thread before any game code runs. Exporting `Unload` as well makes it reloadable while the game runs. Its settings are `<tool>.cfg` files in its folder, embedded at build time. It builds on `native/shared` for its session log, settings, fatal errors, hooks and code patches. A tool that patches a packed executable's code waits for the code to be unpacked, with `ToolHost::addPumpCallback` (the game's first message pump), and checks the bytes it patches before it does.
5. List the tool in the game's `windowFixes.txt`, confirm the fix in play, and add the game to the Games table. The next push to `main` publishes its release.

## Iterating
`windowFixes.session` keeps one game open across commands. It takes the game from `--game`, the install folder from `--gameFolder` or the `windowFixesGameFolder` environment variable, and records the launched process in `windowFixes\session.txt`:

```powershell
$env:windowFixesGameFolder = "<SOMA install folder>"
.venv\Scripts\python.exe -m windowFixes.session --game soma launch
.venv\Scripts\python.exe -m windowFixes.session --game soma log displayGuard
.venv\Scripts\python.exe -m windowFixes.session --game soma reload <tool>.dll
.venv\Scripts\python.exe -m windowFixes.session --game soma capture shot.png
.venv\Scripts\python.exe -m windowFixes.session --game soma close
```

`launch` passes its extra arguments to the game.
