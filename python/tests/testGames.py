import unittest
from pathlib import Path

from windowFixes.games import Game


class TestGames(unittest.TestCase):
  def testSomaRunsSomaExeFromItsInstallFolder(self):
    game = Game("soma", "S:/Games/Steam/steamapps/common/SOMA")
    self.assertEqual(game.executablePath, Path("S:/Games/Steam/steamapps/common/SOMA/Soma.exe"))
    self.assertEqual(game.executableName, "Soma.exe")
    self.assertEqual(game.toolsFolder, Path("S:/Games/Steam/steamapps/common/SOMA/windowFixes"))
    self.assertEqual(game.architecture, "x64")

  def testAnUnknownGameNamesTheKnownOnes(self):
    with self.assertRaisesRegex(ValueError, r"no game named 'nosuchgame'; the games are .*\bsoma\b"):
      Game("nosuchgame", "C:/")


if __name__ == "__main__":
  unittest.main()
