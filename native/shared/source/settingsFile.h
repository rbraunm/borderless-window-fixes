#pragma once

#include <cstddef>
#include <string>
#include <vector>

// key=value settings shared by the tools. A tool's own settings file (loadSettingsFile) is written from the tool's
// embedded default when missing; a defined key it lacks is appended with its default value and the comment above it
// there, so a file from an earlier release keeps working and gains what this one added; and a key the tool does not
// read, such as one a newer release dropped, or a line that is not key=value, is removed from the file and logged with
// its text. A duplicate key or a value that does not parse stops the game, naming the file and line. A sectioned file,
// which players edit while the game runs, rejects unknown keys and reports every problem instead; a key its definition
// marks optional may be absent there, leaving its destination holding what the caller put there.
enum class SettingKind {
  positiveDecimal,   // destination is double*
  positiveInteger,   // destination is unsigned long*
  wholeNumber,       // destination is unsigned long*, zero allowed
  boolean,           // destination is bool*, written as true or false
  text,              // destination is std::string*, any non-empty value
  anyText,           // destination is std::string*, any value, empty included
};

struct SettingDefinition {
  const char* name;
  SettingKind kind;
  void* destination;
  bool isOptional;
};

struct SettingLine {
  std::string key;
  std::string value;
  unsigned long lineNumber;
};

// A [name] header and the key=value lines under it, up to the next header.
struct SettingsSection {
  std::string name;
  unsigned long lineNumber;
  std::vector<SettingLine> lines;
};

struct SectionedSettings {
  std::vector<SettingsSection> sections;
  // Empty when the whole file was read.
  std::string problem;
};

// Reads <toolDirectory>\<fileName>. The tool's embedded default of that name must hold exactly the defined keys.
void loadSettingsFile(const std::string& toolDirectory, const char* fileName,
  const SettingDefinition* definitions, size_t definitionCount);

// The lines of a text file's contents without their line endings; contents ending in a line break end with "".
std::vector<std::string> splitLines(const std::string& contents);

std::string trimWhitespace(const std::string& text);

// Reads the whole file at path into contents, without a UTF-8 byte order mark. Returns a problem naming fileName,
// or an empty string.
std::string readWholeFile(const std::string& path, const std::string& fileName, std::string& contents);

// Reads a file made of [name] sections; the name is everything between the first [ and the last ] of
// its line, and a key=value line before the first header is a problem. fileName names the file.
SectionedSettings readSectionedSettingsFile(const std::string& path, const std::string& fileName);

// Applies key=value lines to definitions under loadSettingsFile's rules. Returns the first problem,
// starting with context, or an empty string.
std::string applySettings(const std::string& context, const std::vector<SettingLine>& lines,
  const SettingDefinition* definitions, size_t definitionCount);

std::string formattedText(const char* format, ...);
