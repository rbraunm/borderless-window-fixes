"""Starts, finds, and closes a game's executable, never taking the foreground from the window in use."""
import os
import subprocess
import time

from windowFixes import windowsApi

showWithoutActivating = 4


class GameError(Exception):
  """The game is not in a state the request needs."""


class GameProcess:
  """One running game executable from an install folder, identified by process ID and creation time, since Windows
  reuses process IDs."""

  def __init__(self, game, processID, startedAt):
    self.game = game
    self.processID = processID
    self.handle = windowsApi.openProcess(processID)
    self.startedAt = windowsApi.processCreationTime(self.handle)
    if startedAt is not None and self.startedAt != startedAt:
      raise GameError(f"process {processID} is not the recorded game: it started at {self.startedAt}, the game at {startedAt}")
    expected = os.path.normcase(str(game.executablePath))
    actual = os.path.normcase(windowsApi.processImagePath(self.handle))
    if actual != expected:
      raise GameError(f"process {processID} runs {actual}, not {expected}")

  @classmethod
  def launch(cls, game, arguments):
    if not game.executablePath.is_file():
      raise GameError(f"{game.executablePath} does not exist")
    running = windowsApi.processIDsNamed(game.executableName)
    if running:
      raise GameError(f"{game.executableName} is already running (process {', '.join(map(str, running))}); close it first")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = showWithoutActivating
    process = subprocess.Popen([str(game.executablePath), *arguments], cwd=game.executablePath.parent, startupinfo=startup)
    return cls(game, process.pid, None)

  @property
  def exitCode(self):
    return windowsApi.exitCode(self.handle)

  def windows(self):
    return windowsApi.visibleWindows(self.processID)

  def isForeground(self):
    return windowsApi.foregroundProcessID() == self.processID

  def close(self, timeoutSeconds):
    """Asks every visible window to close, as their close buttons do, and waits for the process to end."""
    if self.exitCode is not None:
      return self.exitCode
    windows = self.windows()
    if not windows:
      raise GameError("the game has no visible window to close")
    for window, _, _ in windows:
      windowsApi.postClose(window)
    if not windowsApi.waitForExit(self.handle, timeoutSeconds):
      raise GameError(f"the game was still running {timeoutSeconds} seconds after being asked to close")
    return self.exitCode

  def terminate(self, timeoutSeconds):
    if self.exitCode is None:
      windowsApi.terminateProcess(self.handle)
      if not windowsApi.waitForExit(self.handle, timeoutSeconds):
        raise GameError(f"the game was still running {timeoutSeconds} seconds after being terminated")
    return self.exitCode


def waitFor(condition, timeoutSeconds, description):
  deadline = time.monotonic() + timeoutSeconds
  while not condition():
    if time.monotonic() > deadline:
      raise GameError(f"{description} did not happen within {timeoutSeconds} seconds")
    time.sleep(0.25)
