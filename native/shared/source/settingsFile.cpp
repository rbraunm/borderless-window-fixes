#include "settingsFile.h"

#include "defaultFiles.h"
#include "fatalError.h"
#include "sessionLog.h"

#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char utf8ByteOrderMark[] = "\xEF\xBB\xBF";

struct NumberedLine {
  unsigned long lineNumber;
  std::string text;
};

struct DefaultSetting {
  std::string key;
  std::string value;
  std::string comment;
};

std::string formattedText(const char* format, ...) {
  char text[1024];
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  text[sizeof(text) - 1] = '\0';
  return text;
}

std::string trimWhitespace(const std::string& text) {
  const char* const whitespace = " \t\r\n";
  const size_t first = text.find_first_not_of(whitespace);
  if (first == std::string::npos) {
    return "";
  }
  const size_t last = text.find_last_not_of(whitespace);
  return text.substr(first, last - first + 1);
}

std::vector<std::string> splitLines(const std::string& contents) {
  std::vector<std::string> lines;
  size_t lineStart = 0;
  while (lineStart <= contents.size()) {
    size_t lineEnd = contents.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      lineEnd = contents.size();
    }
    std::string line = contents.substr(lineStart, lineEnd - lineStart);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
    lineStart = lineEnd + 1;
  }
  return lines;
}

std::string readWholeFile(const std::string& path, const std::string& fileName, std::string& contents) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return formattedText("Could not open %s: %s", path.c_str(), strerror(errno));
  }
  char chunk[4096];
  size_t bytesRead;
  while ((bytesRead = fread(chunk, 1, sizeof(chunk), file)) > 0) {
    contents.append(chunk, bytesRead);
  }
  const bool readFailed = ferror(file) != 0;
  fclose(file);
  if (readFailed) {
    return formattedText("Could not read %s", fileName.c_str());
  }
  if (contents.compare(0, sizeof(utf8ByteOrderMark) - 1, utf8ByteOrderMark) == 0) {
    contents.erase(0, sizeof(utf8ByteOrderMark) - 1);
  }
  return "";
}

// Trimmed lines that are neither blank nor comments.
static std::vector<NumberedLine> meaningfulLines(const std::string& contents) {
  std::vector<NumberedLine> lines;
  unsigned long lineNumber = 0;
  size_t lineStart = 0;
  while (lineStart <= contents.size()) {
    size_t lineEnd = contents.find('\n', lineStart);
    if (lineEnd == std::string::npos) {
      lineEnd = contents.size();
    }
    const std::string line = trimWhitespace(contents.substr(lineStart, lineEnd - lineStart));
    lineStart = lineEnd + 1;
    lineNumber++;
    if (!line.empty() && line[0] != '#') {
      lines.push_back({ lineNumber, line });
    }
  }
  return lines;
}

static std::string parseSettingLine(const std::string& context, const NumberedLine& line, SettingLine& parsed) {
  const size_t separator = line.text.find('=');
  if (separator == std::string::npos) {
    return formattedText("%s line %lu: expected key=value", context.c_str(), line.lineNumber);
  }
  parsed = { trimWhitespace(line.text.substr(0, separator)), trimWhitespace(line.text.substr(separator + 1)), line.lineNumber };
  return "";
}

static std::string parsePositiveDecimal(const std::string& context, const char* key, const std::string& value, double& parsed) {
  const std::string problem = formattedText("%s: %s must be a positive decimal number, got '%s'", context.c_str(), key, value.c_str());
  if (value.empty() || value.find_first_not_of("0123456789.") != std::string::npos) {
    return problem;
  }
  char* parseEnd = nullptr;
  parsed = strtod(value.c_str(), &parseEnd);
  return *parseEnd != '\0' || !std::isfinite(parsed) || parsed <= 0.0 ? problem : "";
}

static std::string parseWholeNumber(const std::string& context, const char* key, const std::string& value, bool isZeroAllowed,
    unsigned long& parsed) {
  const std::string problem = formattedText(isZeroAllowed ? "%s: %s must be a whole number, got '%s'" :
    "%s: %s must be a positive whole number, got '%s'", context.c_str(), key, value.c_str());
  if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
    return problem;
  }
  errno = 0;
  parsed = strtoul(value.c_str(), nullptr, 10);
  return errno != 0 || (parsed == 0 && !isZeroAllowed) ? problem : "";
}

static std::string parseBoolean(const std::string& context, const char* key, const std::string& value, bool& parsed) {
  if (value == "true" || value == "false") {
    parsed = value == "true";
    return "";
  }
  return formattedText("%s: %s must be true or false, got '%s'", context.c_str(), key, value.c_str());
}

static std::string applyValue(const std::string& context, const SettingDefinition& definition, const std::string& value) {
  switch (definition.kind) {
    case SettingKind::positiveDecimal:
      return parsePositiveDecimal(context, definition.name, value, *static_cast<double*>(definition.destination));
    case SettingKind::positiveInteger:
      return parseWholeNumber(context, definition.name, value, false, *static_cast<unsigned long*>(definition.destination));
    case SettingKind::wholeNumber:
      return parseWholeNumber(context, definition.name, value, true, *static_cast<unsigned long*>(definition.destination));
    case SettingKind::boolean:
      return parseBoolean(context, definition.name, value, *static_cast<bool*>(definition.destination));
    case SettingKind::text:
      if (value.empty()) {
        return formattedText("%s: %s must not be empty", context.c_str(), definition.name);
      }
      *static_cast<std::string*>(definition.destination) = value;
      return "";
    case SettingKind::anyText:
      *static_cast<std::string*>(definition.destination) = value;
      return "";
  }
  return formattedText("%s: %s has an unknown kind", context.c_str(), definition.name);
}

std::string applySettings(const std::string& context, const std::vector<SettingLine>& lines,
    const SettingDefinition* definitions, size_t definitionCount) {
  std::vector<bool> isKeySet(definitionCount, false);
  for (const SettingLine& line : lines) {
    size_t keyIndex = 0;
    while (keyIndex < definitionCount && line.key != definitions[keyIndex].name) {
      keyIndex++;
    }
    if (keyIndex == definitionCount) {
      return formattedText("%s line %lu: unknown key '%s'", context.c_str(), line.lineNumber, line.key.c_str());
    }
    if (isKeySet[keyIndex]) {
      return formattedText("%s line %lu: duplicate key %s", context.c_str(), line.lineNumber, line.key.c_str());
    }
    isKeySet[keyIndex] = true;
    const std::string problem = applyValue(context, definitions[keyIndex], line.value);
    if (!problem.empty()) {
      return problem;
    }
  }
  for (size_t keyIndex = 0; keyIndex < definitionCount; keyIndex++) {
    if (!isKeySet[keyIndex] && !definitions[keyIndex].isOptional) {
      return formattedText("%s: missing required key %s", context.c_str(), definitions[keyIndex].name);
    }
  }
  return "";
}

static size_t findDefinition(const SettingDefinition* definitions, size_t definitionCount, const std::string& key) {
  size_t index = 0;
  while (index < definitionCount && key != definitions[index].name) {
    index++;
  }
  return index;
}

static void applyOrStop(const char* fileName, const SettingDefinition& definition, const std::string& value) {
  const std::string problem = applyValue(fileName, definition, value);
  if (!problem.empty()) {
    fatalError("%s", problem.c_str());
  }
}

// Each key=value line of the embedded default with the comment lines directly above it.
static std::vector<DefaultSetting> readDefaultSettings(const char* fileName, const SettingDefinition* definitions,
    size_t definitionCount) {
  const DefaultFile& defaultFile = findDefaultFile(fileName);
  std::vector<DefaultSetting> settings;
  std::string comment;
  for (const std::string& rawLine : splitLines(std::string(defaultFile.contents, defaultFile.size))) {
    const std::string line = trimWhitespace(rawLine);
    if (line.empty()) {
      comment.clear();
      continue;
    }
    if (line[0] == '#') {
      comment += line + "\r\n";
      continue;
    }
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      fatalError("The default %s carried by this build has a line that is not key=value: '%s'", fileName, line.c_str());
    }
    const std::string key = trimWhitespace(line.substr(0, separator));
    if (findDefinition(definitions, definitionCount, key) == definitionCount) {
      fatalError("The default %s carried by this build has key %s, which the tool does not read", fileName, key.c_str());
    }
    settings.push_back({ key, trimWhitespace(line.substr(separator + 1)), comment });
    comment.clear();
  }
  for (size_t index = 0; index < definitionCount; index++) {
    bool isInDefault = false;
    for (const DefaultSetting& setting : settings) {
      isInDefault = isInDefault || setting.key == definitions[index].name;
    }
    if (!isInDefault) {
      fatalError("The default %s carried by this build has no %s", fileName, definitions[index].name);
    }
  }
  return settings;
}

void loadSettingsFile(const std::string& toolDirectory, const char* fileName,
    const SettingDefinition* definitions, size_t definitionCount) {
  const std::vector<DefaultSetting> defaults = readDefaultSettings(fileName, definitions, definitionCount);
  const std::string path = toolDirectory + "\\" + fileName;
  writeDefaultFileIfMissing(path, fileName);
  std::string contents;
  const std::string readProblem = readWholeFile(path, fileName, contents);
  if (!readProblem.empty()) {
    fatalError("%s", readProblem.c_str());
  }
  std::vector<bool> isKeySet(definitionCount, false);

  const std::vector<std::string> lines = splitLines(contents);
  std::vector<std::string> keptLines;
  bool isChanged = false;
  for (size_t index = 0; index < lines.size(); index++) {
    const unsigned long lineNumber = static_cast<unsigned long>(index + 1);
    const std::string line = trimWhitespace(lines[index]);
    if (line.empty() || line[0] == '#') {
      keptLines.push_back(lines[index]);
      continue;
    }
    const size_t separator = line.find('=');
    const size_t keyIndex = separator == std::string::npos ? definitionCount :
      findDefinition(definitions, definitionCount, trimWhitespace(line.substr(0, separator)));
    if (keyIndex == definitionCount) {
      writeLogLine("WARN", "settingRemoved file=%s line=%lu text=%s", fileName, lineNumber, line.c_str());
      isChanged = true;
      continue;
    }
    if (isKeySet[keyIndex]) {
      fatalError("%s line %lu: duplicate key %s", fileName, lineNumber, definitions[keyIndex].name);
    }
    isKeySet[keyIndex] = true;
    applyOrStop(fileName, definitions[keyIndex], trimWhitespace(line.substr(separator + 1)));
    keptLines.push_back(lines[index]);
  }

  std::string additions;
  for (const DefaultSetting& setting : defaults) {
    const size_t keyIndex = findDefinition(definitions, definitionCount, setting.key);
    if (isKeySet[keyIndex]) {
      continue;
    }
    isKeySet[keyIndex] = true;
    applyOrStop(fileName, definitions[keyIndex], setting.value);
    additions += "\r\n" + setting.comment + setting.key + "=" + setting.value + "\r\n";
    writeLogLine("INFO", "settingAdded file=%s key=%s value=%s", fileName, setting.key.c_str(), setting.value.c_str());
  }
  if (isChanged || !additions.empty()) {
    std::string kept;
    for (size_t index = 0; index < keptLines.size(); index++) {
      kept += keptLines[index] + (index + 1 < keptLines.size() ? "\r\n" : "");
    }
    const bool endsWithLineBreak = kept.empty() || kept.back() == '\n';
    replaceFile(path, kept + (endsWithLineBreak || additions.empty() ? "" : "\r\n") + additions);
  }
}

SectionedSettings readSectionedSettingsFile(const std::string& path, const std::string& fileName) {
  SectionedSettings file;
  std::string contents;
  file.problem = readWholeFile(path, fileName, contents);
  if (!file.problem.empty()) {
    return file;
  }
  for (const NumberedLine& line : meaningfulLines(contents)) {
    if (line.text[0] == '[') {
      const size_t closing = line.text.rfind(']');
      if (closing != line.text.size() - 1) {
        file.problem = formattedText("%s line %lu: a section header must end with ]", fileName.c_str(), line.lineNumber);
        return file;
      }
      file.sections.push_back({ line.text.substr(1, closing - 1), line.lineNumber, {} });
      continue;
    }
    if (file.sections.empty()) {
      file.problem = formattedText("%s line %lu: expected a [name] header before the first key=value line", fileName.c_str(), line.lineNumber);
      return file;
    }
    SettingLine parsed;
    file.problem = parseSettingLine(fileName, line, parsed);
    if (!file.problem.empty()) {
      return file;
    }
    file.sections.back().lines.push_back(parsed);
  }
  return file;
}
