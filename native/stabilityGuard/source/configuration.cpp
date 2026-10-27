#include "configuration.h"

#include "fatalError.h"
#include "settingsFile.h"

static const unsigned long long bytesPerGigabyte = 1024ULL * 1024ULL * 1024ULL;

Configuration loadConfiguration(const std::string& toolDirectory) {
  Configuration configuration = {};
  unsigned long maxDumpGigabytes = 0;
  const SettingDefinition definitions[] = {
    { "maxFullReports", SettingKind::positiveInteger, &configuration.maxFullReports, false },
    { "writeMemoryDumps", SettingKind::boolean, &configuration.writeMemoryDumps, false },
    { "maxDumpGigabytes", SettingKind::wholeNumber, &maxDumpGigabytes, false },
    { "stackScanSlots", SettingKind::positiveInteger, &configuration.stackScanSlots, false },
    { "freezeSeconds", SettingKind::wholeNumber, &configuration.freezeSeconds, false },
    { "exitGuard", SettingKind::boolean, &configuration.exitGuard, false },
    { "exitGraceSeconds", SettingKind::positiveInteger, &configuration.exitGraceSeconds, false },
    { "forceExitHotkey", SettingKind::boolean, &configuration.forceExitHotkey, false },
  };
  loadSettingsFile(toolDirectory, "stabilityGuard.cfg", definitions, sizeof(definitions) / sizeof(definitions[0]));
  configuration.maxDumpBytes = maxDumpGigabytes * bytesPerGigabyte;
  if (configuration.exitGraceSeconds > 600) {
    fatalError("stabilityGuard.cfg: exitGraceSeconds must be at most 600");
  }
  if (configuration.forceExitHotkey && !configuration.exitGuard) {
    fatalError("stabilityGuard.cfg: forceExitHotkey=true needs exitGuard=true");
  }
  if (configuration.stackScanSlots > stackScanSlotLimit) {
    fatalError("stabilityGuard.cfg: stackScanSlots must be at most %lu", stackScanSlotLimit);
  }
  return configuration;
}
