"""Keeps one game open across commands, for iterating on the tools.

  python -m windowFixes.session --game <name> [--gameFolder <folder>] <command> [arguments]

--game names a folder under games. The install folder comes from --gameFolder or the windowFixesGameFolder environment
variable. Options go before the command: everything after it belongs to the command. launch records the process ID
and start time in windowFixes\\session.txt beside the game's executable; the other commands act on that game.
  launch [game arguments...]   start the game's executable without taking the foreground; waits for fixLoader
  status                       process, exit code, windows, foreground
  tools                        the loaded tools and which can be reloaded
  reload <dll>                 reload one tool from the windowFixes folder while the game runs
  log <tool> [INFO|WARN|ERROR] that tool's session log for this game
  capture <png>                save the game's first visible window
  close                        ask the game's windows to close and wait
  kill                         terminate the game
"""
import argparse
import os
import sys

from windowFixes.gameProcess import GameError, GameProcess, waitFor
from windowFixes.games import Game
from windowFixes.linePipe import LoaderSession
from windowFixes.toolLogs import sessionErrors, sessionLog
from windowFixes.windowCapture import captureWindow
from windowFixes.windowsApi import processIDsNamed

sessionFileName = "session.txt"
loaderToolName = "fixLoader"
loaderReadySeconds = 30
closeSeconds = 30


def recordedGame(game):
  path = game.toolsFolder / sessionFileName
  if not path.is_file():
    raise GameError(f"no game was launched from {game.installFolder} ({path} is missing)")
  processID, startedAt = path.read_text(encoding="ascii").split()
  if int(processID) not in processIDsNamed(game.executableName):
    raise GameError(f"the game launched as process {processID} is no longer running")
  return GameProcess(game, int(processID), float(startedAt))


def loaderEvents(process):
  try:
    return [line.event for line in sessionLog(process.game.toolsFolder, loaderToolName, process.processID, process.startedAt)]
  except FileNotFoundError:
    return []


def describeErrors(errors):
  return "; ".join(f"{toolName}: {line.message}" for toolName, line in errors)


def launch(game, arguments):
  process = GameProcess.launch(game, arguments)
  (game.toolsFolder / sessionFileName).write_text(f"{process.processID} {process.startedAt!r}\n", encoding="ascii")
  waitFor(lambda: "loadComplete" in loaderEvents(process) or process.exitCode is not None
    or sessionErrors(game.toolsFolder, process.processID, process.startedAt), loaderReadySeconds, "fixLoader's loadComplete")
  errors = sessionErrors(game.toolsFolder, process.processID, process.startedAt)
  if errors:
    raise GameError(f"a tool failed while loading (its message box is up in process {process.processID}): {describeErrors(errors)}")
  if process.exitCode is not None:
    raise GameError(f"the game exited with {process.exitCode} before fixLoader finished loading")
  print(f"processID={process.processID}")
  for line in sessionLog(game.toolsFolder, loaderToolName, process.processID, process.startedAt):
    print(line)


def status(game):
  process = recordedGame(game)
  print(f"processID={process.processID} exitCode={process.exitCode} foreground={process.isForeground()}")
  for window, title, rectangle in process.windows():
    print(f"window={window} title={title!r} rectangle={rectangle}")


def tools(game):
  process = recordedGame(game)
  session = LoaderSession(process.processID)
  try:
    loaded, reloadable = session.tools()
  finally:
    session.close()
  print(f"loaded={loaded} reloadable={reloadable}")


def reload(game, toolFileName):
  process = recordedGame(game)
  session = LoaderSession(process.processID)
  try:
    print(f"generation={session.reload(toolFileName)}")
  finally:
    session.close()


def log(game, toolName, minimumLevel):
  process = recordedGame(game)
  for line in sessionLog(game.toolsFolder, toolName, process.processID, process.startedAt, minimumLevel):
    print(line)


def capture(game, picturePath):
  process = recordedGame(game)
  windows = process.windows()
  if not windows:
    raise GameError("the game has no visible window")
  captureWindow(windows[0][0]).save(picturePath)
  print(f"saved {picturePath} from window {windows[0][1]!r}")


def main():
  parser = argparse.ArgumentParser(prog="windowFixes.session", description=__doc__.split("\n")[0])
  parser.add_argument("--game", required=True)
  parser.add_argument("--gameFolder", default=os.environ.get("windowFixesGameFolder"))
  parser.add_argument("command", choices=["launch", "status", "tools", "reload", "log", "capture", "close", "kill"])
  parser.add_argument("arguments", nargs=argparse.REMAINDER)
  options = parser.parse_args()
  if not options.gameFolder:
    parser.error("give --gameFolder or set windowFixesGameFolder")
  arguments = options.arguments
  try:
    game = Game(options.game, options.gameFolder)
  except ValueError as error:
    parser.error(str(error))
  try:
    if options.command == "launch":
      launch(game, arguments)
    elif options.command == "status":
      status(game)
    elif options.command == "tools":
      tools(game)
    elif options.command == "reload":
      if len(arguments) != 1:
        parser.error("reload takes one tool DLL name")
      reload(game, arguments[0])
    elif options.command == "log":
      if len(arguments) not in (1, 2):
        parser.error("log takes a tool name and an optional minimum level")
      log(game, arguments[0], arguments[1] if len(arguments) == 2 else "INFO")
    elif options.command == "capture":
      if len(arguments) != 1:
        parser.error("capture takes the output PNG path")
      capture(game, arguments[0])
    elif options.command == "close":
      print(f"exitCode={recordedGame(game).close(closeSeconds)}")
    elif options.command == "kill":
      print(f"exitCode={recordedGame(game).terminate(closeSeconds)}")
  except GameError as error:
    print(f"error: {error}", file=sys.stderr)
    return 1
  return 0


if __name__ == "__main__":
  sys.exit(main())
