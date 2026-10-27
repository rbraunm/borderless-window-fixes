"""A game the repository has fixes for: games\\<name>\\game.cfg names its executable relative to its install folder."""
from pathlib import Path

gamesFolder = Path(__file__).resolve().parents[2] / "games"
toolsFolderName = "windowFixes"


class Game:
  def __init__(self, name, installFolder):
    configPath = gamesFolder / name / "game.cfg"
    if not configPath.is_file():
      known = sorted(path.parent.name for path in gamesFolder.glob("*/game.cfg"))
      raise ValueError(f"no game named {name!r}; the games are {', '.join(known)}")
    executables = []
    for line in configPath.read_text(encoding="ascii").splitlines():
      line = line.strip()
      if line and not line.startswith("#"):
        key, separator, value = line.partition("=")
        if not separator or key.strip() != "executable":
          raise ValueError(f"{configPath}: unknown line {line!r}")
        executables.append(value.strip())
    if len(executables) != 1:
      raise ValueError(f"{configPath} must set executable exactly once")
    self.name = name
    self.installFolder = Path(installFolder)
    self.executablePath = self.installFolder / executables[0]
    self.executableName = self.executablePath.name
    self.toolsFolder = self.executablePath.parent / toolsFolderName
