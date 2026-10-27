"""The tools' command pipes: one command per line, one reply line per command."""
import threading
import time
import urllib.parse

# The game's strings are in the Windows ANSI code page.
textEncoding = "cp1252"
pipeBusyError = 231


class PipeCommandError(Exception):
  """The tool answered with an error, or the pipe failed."""


def encodeArgument(value):
  return urllib.parse.quote(str(value), safe="", encoding=textEncoding)


def parseReply(line):
  """An ok reply's fields; an error reply raises PipeCommandError with its message."""
  if line == "ok":
    return {}
  if line.startswith("ok "):
    fields = {}
    for word in line[3:].split(" "):
      key, separator, value = word.partition("=")
      if not separator or not key:
        raise PipeCommandError(f"malformed field {word!r} in reply {line!r}")
      fields[key] = urllib.parse.unquote(value, encoding=textEncoding, errors="strict")
    return fields
  if line.startswith("error "):
    raise PipeCommandError(line[len("error "):])
  raise PipeCommandError(f"malformed reply {line!r}")


class LinePipeSession:
  """The one connection a tool's pipe in a game accepts. Commands from several threads take turns."""

  pipePrefix = None

  def __init__(self, processID, connectTimeoutSeconds=15):
    self.processID = processID
    self.path = self.pipePrefix + str(processID)
    deadline = time.monotonic() + connectTimeoutSeconds
    while True:
      try:
        self.pipe = open(self.path, "r+b", buffering=0)
        break
      except OSError as error:
        isNotYetServed = isinstance(error, FileNotFoundError) or getattr(error, "winerror", None) == pipeBusyError
        if not isNotYetServed:
          raise
        if time.monotonic() > deadline:
          raise PipeCommandError(f"{self.path} did not open within {connectTimeoutSeconds} seconds") from error
        time.sleep(0.05)
    self.lock = threading.Lock()

  def sendLine(self, line):
    """Sends one raw command line and returns the raw reply line."""
    commandName = line.split(" ", 1)[0]
    with self.lock:
      try:
        self.pipe.write((line + "\n").encode("ascii"))
        reply = b""
        while not reply.endswith(b"\n"):
          chunk = self.pipe.read(4096)
          if not chunk:
            raise PipeCommandError(f"the pipe closed before answering {commandName!r}")
          reply += chunk
      except OSError as error:
        raise PipeCommandError(f"the pipe failed during {commandName!r}: {error}") from error
    return reply.decode("ascii").rstrip("\n")

  def command(self, name, *arguments):
    return parseReply(self.sendLine(" ".join([name] + [encodeArgument(argument) for argument in arguments])))

  def close(self):
    self.pipe.close()


class LoaderSession(LinePipeSession):
  """fixLoader's control pipe: lists the loaded tools and reloads the ones that export Unload."""

  pipePrefix = "\\\\.\\pipe\\fixLoader-"

  def tools(self):
    """({dll: generation} for every loaded tool, [reloadable dll, ...])."""
    fields = self.command("tools")
    reloadable = [name for name in fields.pop("reloadable").split(",") if name]
    return {name: int(generation) for name, generation in fields.items()}, reloadable

  def reload(self, toolFileName):
    """Unloads the tool and loads the DLL now in the windowFixes folder; returns its new generation."""
    return int(self.command("reload", toolFileName)["generation"])
