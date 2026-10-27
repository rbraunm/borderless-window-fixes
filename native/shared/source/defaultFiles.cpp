#include "defaultFiles.h"

#include "fatalError.h"
#include "sessionLog.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

// A game starting at the same moment may be reading the file being replaced.
static const int replaceAttempts = 40;
static const DWORD replaceRetryMilliseconds = 50;

const DefaultFile& findDefaultFile(const char* fileName) {
  for (size_t index = 0; index < defaultFileCount; index++) {
    if (strcmp(defaultFiles[index].fileName, fileName) == 0) {
      return defaultFiles[index];
    }
  }
  fatalError("This build carries no default %s", fileName);
}

static std::string writeStagedFile(const std::string& path, const char* contents, size_t size) {
  char processSuffix[32];
  snprintf(processSuffix, sizeof(processSuffix), ".%lu.new", GetCurrentProcessId());
  const std::string stagedPath = path + processSuffix;
  FILE* file = fopen(stagedPath.c_str(), "wb");
  if (file == nullptr) {
    fatalError("Could not write %s (error %lu)", stagedPath.c_str(), GetLastError());
  }
  const size_t written = fwrite(contents, 1, size, file);
  const bool isClosed = fclose(file) == 0;
  if (written != size || !isClosed) {
    fatalError("Could not write all of %s", stagedPath.c_str());
  }
  return stagedPath;
}

static const char* fileNameOf(const std::string& path) {
  const size_t separator = path.find_last_of("\\/");
  return separator == std::string::npos ? path.c_str() : path.c_str() + separator + 1;
}

bool writeDefaultFileIfMissing(const std::string& path, const char* defaultFileName) {
  if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  const DefaultFile& defaultFile = findDefaultFile(defaultFileName);
  const std::string stagedPath = writeStagedFile(path, defaultFile.contents, defaultFile.size);
  // Without MOVEFILE_REPLACE_EXISTING the move fails when another game created the file first, which keeps that one.
  if (!MoveFileExA(stagedPath.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
    const DWORD error = GetLastError();
    DeleteFileA(stagedPath.c_str());
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
      return false;
    }
    fatalError("Could not create %s (error %lu)", path.c_str(), error);
  }
  writeLogLine("INFO", "settingsFileCreated file=%s", fileNameOf(path));
  return true;
}

void replaceFile(const std::string& path, const std::string& contents) {
  const std::string stagedPath = writeStagedFile(path, contents.data(), contents.size());
  for (int attempt = 1; ; attempt++) {
    if (MoveFileExA(stagedPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return;
    }
    const DWORD error = GetLastError();
    if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) || attempt == replaceAttempts) {
      DeleteFileA(stagedPath.c_str());
      fatalError("Could not replace %s (error %lu)", path.c_str(), error);
    }
    Sleep(replaceRetryMilliseconds);
  }
}
