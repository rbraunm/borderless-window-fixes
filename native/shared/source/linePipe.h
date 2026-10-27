#pragma once

#include <windows.h>

#include <functional>
#include <string>

// Serves a named pipe on a thread of its own: one client at a time, one command per line in, one reply line out (the
// handler's return, without the line ending).
class LinePipeServer {
public:
  LinePipeServer() = default;
  LinePipeServer(const LinePipeServer&) = delete;
  LinePipeServer& operator=(const LinePipeServer&) = delete;

  // \\.\pipe\<baseName>-<process ID>.
  void startForProcess(const char* baseName, const std::function<std::string(const std::string&)>& handler);
  // \\.\pipe\<name>, which no other process may be serving.
  void startNamed(const char* name, const std::function<std::string(const std::string&)>& handler);
  // Finishes the client being served, if any, and ends the pipe until it is started again; the name is free once it
  // returns. Never call it from the handler.
  void stop();

private:
  static DWORD WINAPI serve(LPVOID server);
  void start(const std::function<std::string(const std::string&)>& handler);
  void serveClient(HANDLE pipe);

  std::function<std::string(const std::string&)> lineHandler;
  char pipeName[96] = {};
  HANDLE serverThread = nullptr;
  volatile LONG isStopping = 0;
};
