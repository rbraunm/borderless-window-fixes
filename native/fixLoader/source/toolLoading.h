#pragma once

#include "toolHost.h"

#include <string>

// Loads every DLL named in <host.toolDirectory>\windowFixes.txt, in file order, each from a shadow
// copy under <host.toolDirectory>\loaded, and calls its Load export with the host on the current
// (main) thread. Shadow copies left by games that have ended are removed first. The list is written from the
// loader's embedded default when missing and brought up to this release's tools (readToolList). Any other
// problem is fatal.
void loadListedTools(const ToolHost& host);

// The control pipe's commands, one line each:
//   tools          -> ok <dll>=<generation> ... reloadable=<dll>,<dll>
//   reload <dll>   -> ok generation=<n> milliseconds=<n>: Unload, free, then load the DLL now in the
//                     windowFixes folder from a fresh shadow copy
std::string runLoaderCommand(const std::string& line);
