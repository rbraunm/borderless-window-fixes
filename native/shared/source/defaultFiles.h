#pragma once

#include <cstddef>
#include <string>

// A settings file the tool ships, embedded at build time by cmake/embedDefaultFiles.cmake with CRLF line endings.
struct DefaultFile {
  const char* fileName;
  const char* contents;
  size_t size;
};

extern const DefaultFile defaultFiles[];
extern const size_t defaultFileCount;

// The tool's embedded copy of fileName. Fatal when the tool ships no such file.
const DefaultFile& findDefaultFile(const char* fileName);

// Writes the embedded copy of defaultFileName to path unless a file is already there; a game starting at the same
// moment never sees it half written. Returns true when it wrote the file.
bool writeDefaultFileIfMissing(const std::string& path, const char* defaultFileName);

// Replaces the file at path with contents in one step, waiting a moment for another game reading it.
void replaceFile(const std::string& path, const std::string& contents);
