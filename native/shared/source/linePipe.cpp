#include "linePipe.h"

#include "fatalError.h"
#include "sessionLog.h"

#include <cstdio>

namespace {

const size_t maximumLineLength = 4096;

bool writeReply(HANDLE pipe, const std::string& reply) {
  const std::string line = reply + "\n";
  DWORD written = 0;
  return WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) && written == line.size();
}

}

// Ends when the client disconnects or a write fails.
void LinePipeServer::serveClient(HANDLE pipe) {
  std::string pending;
  char buffer[1024];
  for (;;) {
    DWORD received = 0;
    if (!ReadFile(pipe, buffer, sizeof(buffer), &received, nullptr) || received == 0) {
      return;
    }
    pending.append(buffer, received);
    for (size_t lineEnd = pending.find('\n'); lineEnd != std::string::npos; lineEnd = pending.find('\n')) {
      std::string line = pending.substr(0, lineEnd);
      pending.erase(0, lineEnd + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!writeReply(pipe, lineHandler(line))) {
        return;
      }
    }
    if (pending.size() > maximumLineLength) {
      writeLogLine("WARN", "commandTooLong bytes=%zu", pending.size());
      writeReply(pipe, "error a command line is at most " + std::to_string(maximumLineLength) + " bytes");
      return;
    }
  }
}

DWORD WINAPI LinePipeServer::serve(LPVOID parameter) {
  LinePipeServer& server = *static_cast<LinePipeServer*>(parameter);
  bool isFirst = true;
  while (InterlockedCompareExchange(&server.isStopping, 0, 0) == 0) {
    // The first instance is made with FILE_FLAG_FIRST_PIPE_INSTANCE, so a name another process serves fails here
    // instead of the two sharing it.
    HANDLE pipe = CreateNamedPipeA(server.pipeName, PIPE_ACCESS_DUPLEX | (isFirst ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      fatalError("Could not create %s (error %lu)", server.pipeName, GetLastError());
    }
    isFirst = false;
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED) {
      fatalError("Could not accept a client on %s (error %lu)", server.pipeName, GetLastError());
    }
    if (InterlockedCompareExchange(&server.isStopping, 0, 0) == 0) {
      server.serveClient(pipe);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
  return 0;
}

void LinePipeServer::stop() {
  if (serverThread == nullptr) {
    return;
  }
  InterlockedExchange(&isStopping, 1);
  // The thread waits in ConnectNamedPipe for a client; connecting as one wakes it, and it ends without making the pipe
  // again. While it serves another client, or between pipes, the connection is tried again.
  while (WaitForSingleObject(serverThread, 0) == WAIT_TIMEOUT) {
    const HANDLE client = CreateFileA(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (client != INVALID_HANDLE_VALUE) {
      CloseHandle(client);
      WaitForSingleObject(serverThread, 1000);
      continue;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_BUSY) {
      WaitNamedPipeA(pipeName, 100);
    } else if (error == ERROR_FILE_NOT_FOUND) {
      Sleep(10);
    } else {
      fatalError("Could not wake %s to stop it (error %lu)", pipeName, error);
    }
  }
  CloseHandle(serverThread);
  serverThread = nullptr;
  writeLogLine("INFO", "commandPipeStopped pipe=%s", pipeName);
}

void LinePipeServer::start(const std::function<std::string(const std::string&)>& handler) {
  if (serverThread != nullptr) {
    fatalError("%s is served already", pipeName);
  }
  lineHandler = handler;
  InterlockedExchange(&isStopping, 0);
  serverThread = CreateThread(nullptr, 0, &LinePipeServer::serve, this, 0, nullptr);
  if (serverThread == nullptr) {
    fatalError("Could not start the thread serving %s (error %lu)", pipeName, GetLastError());
  }
  writeLogLine("INFO", "commandPipeStarted pipe=%s", pipeName);
}

void LinePipeServer::startForProcess(const char* baseName, const std::function<std::string(const std::string&)>& handler) {
  snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\%s-%lu", baseName, GetCurrentProcessId());
  start(handler);
}

void LinePipeServer::startNamed(const char* name, const std::function<std::string(const std::string&)>& handler) {
  snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\%s", name);
  start(handler);
}
