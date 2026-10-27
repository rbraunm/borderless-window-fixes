#include "toolLoading.h"

#include "defaultFiles.h"
#include "fatalError.h"
#include "sessionLog.h"
#include "settingsFile.h"

#include <windows.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char* const toolListFileName = "windowFixes.txt";
static const char* const shadowFolderName = "loaded";
static const char dllExtension[] = ".dll";

struct LoadedTool {
  std::string fileName;
  unsigned long generation;
  std::string shadowPath;
  HMODULE module;
  ToolUnloadFunction unload;
};

static ToolHost toolHost;
static std::string toolDirectory;
static std::string shadowDirectory;
static std::vector<LoadedTool> loadedTools;

static bool hasDllExtension(const std::string& name) {
  const size_t extensionLength = sizeof(dllExtension) - 1;
  return name.size() > extensionLength &&
    _stricmp(name.c_str() + name.size() - extensionLength, dllExtension) == 0;
}

static bool isToolFileName(const std::string& name) {
  return name.find_first_of("\\/: ") == std::string::npos && hasDllExtension(name);
}

struct ToolEntry {
  std::string fileName;
  bool isOn;
  size_t lineIndex;
};

enum class ToolLineKind {
  blankOrComment,
  tool,
  unusable,
};

static bool isInToolDirectory(const std::string& fileName);

// A line names a tool, switched off when it starts with #; any other line starting with # is a comment, and any other
// line is unusable.
static ToolLineKind readToolEntry(const std::string& rawLine, size_t lineIndex, ToolEntry* entry) {
  const std::string line = trimWhitespace(rawLine);
  if (line.empty()) {
    return ToolLineKind::blankOrComment;
  }
  if (line[0] != '#') {
    if (!isToolFileName(line)) {
      return ToolLineKind::unusable;
    }
    *entry = { line, true, lineIndex };
    return ToolLineKind::tool;
  }
  const std::string name = trimWhitespace(line.substr(1));
  if (!isToolFileName(name)) {
    return ToolLineKind::blankOrComment;
  }
  *entry = { name, false, lineIndex };
  return ToolLineKind::tool;
}

static const ToolEntry* findEntry(const std::vector<ToolEntry>& entries, const std::string& fileName) {
  for (const ToolEntry& entry : entries) {
    if (_stricmp(entry.fileName.c_str(), fileName.c_str()) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

// The tools to load, in list order. Every tool the shipped list names stays in the file, on or switched off: one the
// file lacks, such as a tool a newer release added, goes in with its shipped state after the nearest tool listed
// before it there, so the load order stays as shipped. A line naming a tool the release does not ship and the folder
// does not hold, such as one a newer release dropped, is removed and logged, and so is a line that names no tool.
static std::vector<std::string> readToolList() {
  const std::string listPath = toolDirectory + "\\" + toolListFileName;
  writeDefaultFileIfMissing(listPath, toolListFileName);

  const DefaultFile& shippedList = findDefaultFile(toolListFileName);
  const std::vector<std::string> shippedLines = splitLines(std::string(shippedList.contents, shippedList.size));
  std::vector<ToolEntry> shippedEntries;
  for (size_t index = 0; index < shippedLines.size(); index++) {
    ToolEntry entry;
    if (readToolEntry(shippedLines[index], index, &entry) == ToolLineKind::tool) {
      shippedEntries.push_back(entry);
    }
  }

  std::string contents;
  const std::string readProblem = readWholeFile(listPath, toolListFileName, contents);
  if (!readProblem.empty()) {
    fatalError("%s", readProblem.c_str());
  }
  const std::vector<std::string> fileLines = splitLines(contents);
  std::vector<std::string> lines;
  std::vector<ToolEntry> entries;
  bool isChanged = false;
  for (size_t index = 0; index < fileLines.size(); index++) {
    ToolEntry entry;
    const ToolLineKind kind = readToolEntry(fileLines[index], lines.size(), &entry);
    if (kind == ToolLineKind::unusable || (kind == ToolLineKind::tool &&
        findEntry(shippedEntries, entry.fileName) == nullptr && !isInToolDirectory(entry.fileName))) {
      writeLogLine("WARN", "toolRemoved file=%s line=%zu text=%s", toolListFileName, index + 1,
        trimWhitespace(fileLines[index]).c_str());
      isChanged = true;
      continue;
    }
    if (kind == ToolLineKind::tool) {
      if (findEntry(entries, entry.fileName) != nullptr) {
        fatalError("%s line %zu: %s is listed twice", toolListFileName, index + 1, entry.fileName.c_str());
      }
      entries.push_back(entry);
    }
    lines.push_back(fileLines[index]);
  }

  for (size_t shippedIndex = 0; shippedIndex < shippedEntries.size(); shippedIndex++) {
    const ToolEntry& shipped = shippedEntries[shippedIndex];
    if (findEntry(entries, shipped.fileName) != nullptr) {
      continue;
    }
    size_t insertAt = !lines.empty() && lines.back().empty() ? lines.size() - 1 : lines.size();
    if (!entries.empty()) {
      insertAt = entries.front().lineIndex;
      for (const ToolEntry& entry : entries) {
        insertAt = entry.lineIndex < insertAt ? entry.lineIndex : insertAt;
      }
    }
    for (size_t earlier = shippedIndex; earlier > 0; earlier--) {
      const ToolEntry* listed = findEntry(entries, shippedEntries[earlier - 1].fileName);
      if (listed != nullptr) {
        insertAt = listed->lineIndex + 1;
        break;
      }
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), (shipped.isOn ? "" : "#") + shipped.fileName);
    for (ToolEntry& entry : entries) {
      entry.lineIndex += entry.lineIndex >= insertAt ? 1 : 0;
    }
    entries.push_back({ shipped.fileName, shipped.isOn, insertAt });
    writeLogLine("INFO", "toolAdded file=%s tool=%s on=%s", toolListFileName, shipped.fileName.c_str(),
      shipped.isOn ? "true" : "false");
    isChanged = true;
  }
  if (isChanged) {
    std::string newContents;
    for (size_t index = 0; index < lines.size(); index++) {
      newContents += lines[index] + (index + 1 < lines.size() ? "\r\n" : "");
    }
    replaceFile(listPath, newContents);
  }

  std::vector<std::string> toolNames;
  for (size_t index = 0; index < lines.size(); index++) {
    for (const ToolEntry& entry : entries) {
      if (entry.lineIndex == index && entry.isOn) {
        toolNames.push_back(entry.fileName);
      }
    }
  }
  return toolNames;
}

// The DLL loader takes long paths only through the wide calls, even with the process marked long-path aware.
static std::wstring widePath(const std::string& path) {
  const int length = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
  if (length <= 0) {
    fatalError("Could not convert the path %s (error %lu)", path.c_str(), GetLastError());
  }
  std::wstring converted(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path.c_str(), static_cast<int>(path.size()), &converted[0], length);
  return converted;
}

static bool isInToolDirectory(const std::string& fileName) {
  return GetFileAttributesA((toolDirectory + "\\" + fileName).c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::string shadowPathFor(const std::string& fileName, unsigned long generation) {
  const std::string stem = fileName.substr(0, fileName.size() - (sizeof(dllExtension) - 1));
  char suffix[64];
  snprintf(suffix, sizeof(suffix), ".%lu.%lu%s", GetCurrentProcessId(), generation, dllExtension);
  return shadowDirectory + "\\" + stem + suffix;
}

// The process ID in a shadow copy's name, <tool>.<process ID>.<generation>.dll; zero for any other name.
static DWORD processOfShadowCopy(const std::string& name) {
  const size_t stemEnd = name.size() - (sizeof(dllExtension) - 1);
  const size_t generationDot = name.rfind('.', stemEnd - 1);
  if (generationDot == std::string::npos || generationDot == 0) {
    return 0;
  }
  const size_t processDot = name.rfind('.', generationDot - 1);
  if (processDot == std::string::npos) {
    return 0;
  }
  const std::string digits = name.substr(processDot + 1, generationDot - processDot - 1);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
    return 0;
  }
  return strtoul(digits.c_str(), nullptr, 10);
}

static bool isProcessRunning(DWORD processID) {
  const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processID);
  if (process == nullptr) {
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  const bool isRunning = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return isRunning;
}

// Shadow copies are named <tool>.<process ID>.<generation>.dll. Only the copies of games that have ended are
// removed: a game starting at the same moment holds its fresh copies open only once it has loaded them.
static void prepareShadowDirectory() {
  shadowDirectory = toolDirectory + "\\" + shadowFolderName;
  if (!CreateDirectoryA(shadowDirectory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    fatalError("Could not create the %s folder for the tools' shadow copies (error %lu)", shadowFolderName, GetLastError());
  }
  const std::string searchPattern = shadowDirectory + "\\*" + dllExtension;
  WIN32_FIND_DATAA foundFile;
  HANDLE searchHandle = FindFirstFileA(searchPattern.c_str(), &foundFile);
  if (searchHandle == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND) {
      return;
    }
    fatalError("Could not list the shadow copies (error %lu)", GetLastError());
  }
  do {
    const std::string path = shadowDirectory + "\\" + foundFile.cFileName;
    const DWORD owner = processOfShadowCopy(foundFile.cFileName);
    if (owner != 0 && isProcessRunning(owner)) {
      writeLogLine("INFO", "shadowCopyKept file=%s reason=running", foundFile.cFileName);
    } else if (DeleteFileA(path.c_str())) {
      writeLogLine("INFO", "shadowCopyRemoved file=%s", foundFile.cFileName);
    } else {
      writeLogLine("INFO", "shadowCopyKept file=%s error=%lu", foundFile.cFileName, GetLastError());
    }
  } while (FindNextFileA(searchHandle, &foundFile));
  const DWORD enumerationError = GetLastError();
  FindClose(searchHandle);
  if (enumerationError != ERROR_NO_MORE_FILES) {
    fatalError("Listing the shadow copies stopped early (error %lu)", enumerationError);
  }
}

// Copies the tool's DLL to a fresh shadow copy, loads that, and calls Load, whose own failures are
// fatal inside the tool. Every other failure is fatal here.
static LoadedTool loadTool(const std::string& fileName, unsigned long generation) {
  const std::string sourcePath = toolDirectory + "\\" + fileName;
  const std::string shadowPath = shadowPathFor(fileName, generation);
  if (!CopyFileA(sourcePath.c_str(), shadowPath.c_str(), FALSE)) {
    fatalError("Could not copy %s to its shadow copy (error %lu)", fileName.c_str(), GetLastError());
  }
  HMODULE module = LoadLibraryExW((L"\\\\?\\" + widePath(shadowPath)).c_str(), nullptr, 0);
  if (module == nullptr) {
    fatalError("Could not load %s (error %lu)", fileName.c_str(), GetLastError());
  }
  ToolLoadFunction load = reinterpret_cast<ToolLoadFunction>(GetProcAddress(module, "Load"));
  if (load == nullptr) {
    fatalError("%s does not export Load", fileName.c_str());
  }
  const DWORD startTicks = GetTickCount();
  const DWORD result = load(&toolHost);
  if (result != 0) {
    fatalError("%s Load returned %lu", fileName.c_str(), result);
  }
  const LoadedTool loaded = { fileName, generation, shadowPath, module,
    reinterpret_cast<ToolUnloadFunction>(GetProcAddress(module, "Unload")) };
  writeLogLine("INFO", "toolLoaded name=%s generation=%lu reloadable=%s milliseconds=%lu", fileName.c_str(), generation,
    loaded.unload != nullptr ? "true" : "false", GetTickCount() - startTicks);
  return loaded;
}

void loadListedTools(const ToolHost& host) {
  toolDirectory = host.toolDirectory;
  toolHost = host;
  toolHost.toolDirectory = toolDirectory.c_str();
  prepareShadowDirectory();
  const std::vector<std::string> toolNames = readToolList();
  if (toolNames.empty()) {
    writeLogLine("WARN", "noToolsListed file=%s", toolListFileName);
    return;
  }
  for (const std::string& toolName : toolNames) {
    if (!isInToolDirectory(toolName)) {
      fatalError("%s lists %s, but it is not in the windowFixes folder", toolListFileName, toolName.c_str());
    }
    loadedTools.push_back(loadTool(toolName, 1));
  }
}

static std::string reloadTool(const std::string& fileName) {
  LoadedTool* tool = nullptr;
  for (LoadedTool& loaded : loadedTools) {
    if (_stricmp(loaded.fileName.c_str(), fileName.c_str()) == 0) {
      tool = &loaded;
    }
  }
  if (tool == nullptr) {
    return "error " + fileName + " is not a loaded tool";
  }
  if (tool->unload == nullptr) {
    return "error " + tool->fileName + " does not export Unload, so it loads only at game start";
  }
  if (!isInToolDirectory(tool->fileName)) {
    return "error " + tool->fileName + " is not in the windowFixes folder";
  }
  const DWORD startTicks = GetTickCount();
  const DWORD unloadResult = tool->unload();
  if (unloadResult != 0) {
    fatalError("%s Unload returned %lu", tool->fileName.c_str(), unloadResult);
  }
  if (!FreeLibrary(tool->module)) {
    fatalError("Could not free %s (error %lu)", tool->fileName.c_str(), GetLastError());
  }
  HMODULE stillLoaded = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(tool->module), &stillLoaded)) {
    fatalError("%s is still loaded after Unload and FreeLibrary; something else holds a reference to it", tool->fileName.c_str());
  }
  if (!DeleteFileA(tool->shadowPath.c_str())) {
    fatalError("Could not remove the unloaded shadow copy of %s (error %lu)", tool->fileName.c_str(), GetLastError());
  }
  writeLogLine("INFO", "toolUnloaded name=%s generation=%lu", tool->fileName.c_str(), tool->generation);
  *tool = loadTool(tool->fileName, tool->generation + 1);
  return "ok generation=" + std::to_string(tool->generation) + " milliseconds=" + std::to_string(GetTickCount() - startTicks);
}

static std::string listTools() {
  std::string reply = "ok";
  std::string reloadable;
  for (const LoadedTool& tool : loadedTools) {
    reply += " " + tool.fileName + "=" + std::to_string(tool.generation);
    if (tool.unload != nullptr) {
      reloadable += (reloadable.empty() ? "" : ",") + tool.fileName;
    }
  }
  return reply + " reloadable=" + reloadable;
}

std::string runLoaderCommand(const std::string& line) {
  const size_t space = line.find(' ');
  const std::string name = line.substr(0, space);
  const std::string argument = space == std::string::npos ? "" : line.substr(space + 1);
  std::string reply;
  if (name == "tools") {
    reply = argument.empty() ? listTools() : "error tools takes no arguments";
  } else if (name == "reload") {
    reply = argument.empty() || argument.find(' ') != std::string::npos
      ? "error reload takes one tool DLL name" : reloadTool(argument);
  } else {
    reply = "error unknown command '" + name + "'";
  }
  writeLogLine(reply.compare(0, 2, "ok") == 0 ? "INFO" : "WARN", "command line=%s reply=%s", line.c_str(), reply.c_str());
  return reply;
}
