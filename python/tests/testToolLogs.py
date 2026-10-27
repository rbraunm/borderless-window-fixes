import shutil
import tempfile
import unittest
from datetime import datetime
from pathlib import Path

from windowFixes.toolLogs import LogLine, sessionErrors, sessionLog, sessionLogPath


def epochSeconds(text):
  return datetime.strptime(text, "%Y-%m-%d %H:%M:%S").timestamp()


class TestSessionLogs(unittest.TestCase):
  def setUp(self):
    self.toolsFolder = Path(tempfile.mkdtemp(prefix="windowFixesLogs"))
    self.addCleanup(shutil.rmtree, self.toolsFolder)

  def writeLog(self, toolName, fileName, *lines):
    directory = self.toolsFolder / "logs" / toolName
    directory.mkdir(parents=True, exist_ok=True)
    (directory / fileName).write_text("".join(line + "\r\n" for line in lines), encoding="ascii")

  def testTheLogIsFoundByProcessID(self):
    self.writeLog("fixLoader", "fixLoader_20261001_184505_pid23064.log", "2026-10-01 18:45:05.385 INFO sessionStart version=0.1.0")
    self.writeLog("fixLoader", "fixLoader_20261001_184135_pid5152.log", "2026-10-01 18:41:35.444 INFO sessionStart version=0.1.0")
    path = sessionLogPath(self.toolsFolder, "fixLoader", 5152, epochSeconds("2026-10-01 18:41:34"))
    self.assertEqual(path.name, "fixLoader_20261001_184135_pid5152.log")

  def testAReusedProcessIDPicksTheLogOfTheGameThatStartedThen(self):
    self.writeLog("fixLoader", "fixLoader_20260801_100000_pid1660.log", "2026-08-01 10:00:00.000 INFO loadComplete")
    self.writeLog("fixLoader", "fixLoader_20261003_210341_pid1660.log", "2026-10-03 21:03:41.139 INFO sessionStart version=0.1.0")
    path = sessionLogPath(self.toolsFolder, "fixLoader", 1660, epochSeconds("2026-10-03 21:03:40"))
    self.assertEqual(path.name, "fixLoader_20261003_210341_pid1660.log")

  def testAnOlderGamesLogDoesNotCountForANewGameWithTheSameProcessID(self):
    self.writeLog("fixLoader", "fixLoader_20260801_100000_pid1660.log", "2026-08-01 10:00:00.000 INFO loadComplete")
    with self.assertRaises(FileNotFoundError):
      sessionLogPath(self.toolsFolder, "fixLoader", 1660, epochSeconds("2026-10-03 21:03:40"))

  def testAProcessWithoutALogIsAnError(self):
    with self.assertRaises(FileNotFoundError):
      sessionLogPath(self.toolsFolder, "fixLoader", 77, epochSeconds("2026-10-01 00:00:00"))

  def testProcessIDsAreMatchedWhole(self):
    self.writeLog("fixLoader", "fixLoader_20261001_184135_pid15152.log", "2026-10-01 18:41:35.444 INFO sessionStart version=0.1.0")
    with self.assertRaises(FileNotFoundError):
      sessionLogPath(self.toolsFolder, "fixLoader", 5152, epochSeconds("2026-10-01 18:41:34"))

  def testMinimumLevelFiltersLines(self):
    self.writeLog("fixLoader", "fixLoader_20261001_184135_pid5152.log",
      "2026-10-01 18:41:35.444 INFO sessionStart version=0.1.0",
      "2026-10-01 18:41:36.000 WARN noToolsListed file=windowFixes.txt",
      "2026-10-01 18:41:37.000 ERROR could not load")
    lines = sessionLog(self.toolsFolder, "fixLoader", 5152, epochSeconds("2026-10-01 18:41:34"), "WARN")
    self.assertEqual([line.event for line in lines], ["noToolsListed", "could"])

  def testAnUnknownLevelIsAnError(self):
    self.writeLog("fixLoader", "fixLoader_20261001_184135_pid5152.log", "2026-10-01 18:41:35.444 INFO sessionStart version=0.1.0")
    with self.assertRaises(ValueError):
      sessionLog(self.toolsFolder, "fixLoader", 5152, epochSeconds("2026-10-01 18:41:34"), "DEBUG")

  def testErrorsAreCollectedFromEveryToolOfTheGame(self):
    self.writeLog("fixLoader", "fixLoader_20261001_184135_pid5152.log", "2026-10-01 18:41:35.444 INFO sessionStart version=0.1.0")
    self.writeLog("directConnect", "directConnect_20261001_184135_pid5152.log",
      "2026-10-01 18:41:35.500 INFO sessionStart version=0.1.0",
      "2026-10-01 18:41:35.501 ERROR Windows Credential Manager has no generic credential 'x'")
    self.writeLog("directConnect", "directConnect_20260901_120000_pid5152.log", "2026-09-01 12:00:00.000 ERROR an older game's failure")
    errors = sessionErrors(self.toolsFolder, 5152, epochSeconds("2026-10-01 18:41:34"))
    self.assertEqual([(toolName, line.message) for toolName, line in errors],
      [("directConnect", "Windows Credential Manager has no generic credential 'x'")])


class TestLogLines(unittest.TestCase):
  def testEventAndFieldsAreSplitOut(self):
    line = LogLine("2026-10-01 18:41:35.444 INFO argumentsSupplied server=pyreal port=9200")
    self.assertEqual((line.level, line.event, line.fields), ("INFO", "argumentsSupplied", {"server": "pyreal", "port": "9200"}))

  def testAMalformedLineIsAnError(self):
    with self.assertRaises(ValueError):
      LogLine("not a log line")


if __name__ == "__main__":
  unittest.main()
