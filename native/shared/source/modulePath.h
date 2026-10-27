#pragma once

#include <windows.h>

#include <string>

// Directory that contains the given module, without a trailing separator.
std::string resolveModuleDirectory(HMODULE module);
