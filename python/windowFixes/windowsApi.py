"""The few Win32 calls the client needs, through ctypes."""
import ctypes
from ctypes import wintypes

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

windowMessageClose = 0x0010
snapshotProcesses = 0x00000002
invalidHandle = wintypes.HANDLE(-1).value
enumWindowsProcedure = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [enumWindowsProcedure, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.GetForegroundWindow.restype = wintypes.HWND
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE


class PROCESSENTRY32W(ctypes.Structure):
  _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
    ("szExeFile", ctypes.c_wchar * 260)]


def raiseLastError(call):
  raise ctypes.WinError(ctypes.get_last_error(), f"{call} failed")


def processIDsNamed(executableName):
  snapshot = kernel32.CreateToolhelp32Snapshot(snapshotProcesses, 0)
  if snapshot == invalidHandle:
    raiseLastError("CreateToolhelp32Snapshot")
  try:
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    found = []
    hasEntry = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
    while hasEntry:
      if entry.szExeFile.lower() == executableName.lower():
        found.append(entry.th32ProcessID)
      hasEntry = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    return found
  finally:
    kernel32.CloseHandle(snapshot)


def visibleWindows(processID):
  """The process's visible top-level windows as (handle, title, (left, top, right, bottom))."""
  found = []

  def visit(window, _):
    owner = wintypes.DWORD()
    user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
    if owner.value == processID and user32.IsWindowVisible(window):
      title = ctypes.create_unicode_buffer(256)
      user32.GetWindowTextW(window, title, len(title))
      rectangle = wintypes.RECT()
      user32.GetWindowRect(window, ctypes.byref(rectangle))
      found.append((window, title.value, (rectangle.left, rectangle.top, rectangle.right, rectangle.bottom)))
    return True

  user32.EnumWindows(enumWindowsProcedure(visit), 0)
  return found


def foregroundProcessID():
  owner = wintypes.DWORD()
  user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), ctypes.byref(owner))
  return owner.value


def postClose(window):
  if not user32.PostMessageW(window, windowMessageClose, 0, 0):
    raiseLastError("PostMessageW")



processQueryLimitedInformation = 0x1000
processTerminate = 0x0001
synchronize = 0x00100000
stillActive = 259
waitTimeout = 0x00000102
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def openProcess(processID):
  handle = kernel32.OpenProcess(processQueryLimitedInformation | processTerminate | synchronize, False, processID)
  if not handle:
    raiseLastError(f"OpenProcess({processID})")
  return handle


def exitCode(processHandle):
  """None while the process runs."""
  code = wintypes.DWORD()
  if not kernel32.GetExitCodeProcess(processHandle, ctypes.byref(code)):
    raiseLastError("GetExitCodeProcess")
  return None if code.value == stillActive else code.value


waitObject0 = 0x00000000


def waitForExit(processHandle, timeoutSeconds):
  """True once the process has ended, False when the wait timed out."""
  result = kernel32.WaitForSingleObject(processHandle, int(timeoutSeconds * 1000))
  if result == waitObject0:
    return True
  if result == waitTimeout:
    return False
  raiseLastError("WaitForSingleObject")


def terminateProcess(processHandle):
  if not kernel32.TerminateProcess(processHandle, 1):
    raiseLastError("TerminateProcess")


kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
kernel32.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
fileTimeTicksAtUnixEpoch = 116444736000000000


def processCreationTime(processHandle):
  """Seconds since the Unix epoch, as a float."""
  times = [wintypes.FILETIME() for _ in range(4)]
  if not kernel32.GetProcessTimes(processHandle, *[ctypes.byref(time) for time in times]):
    raiseLastError("GetProcessTimes")
  ticks = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
  return (ticks - fileTimeTicksAtUnixEpoch) / 10_000_000


def processImagePath(processHandle):
  size = wintypes.DWORD(32768)
  buffer = ctypes.create_unicode_buffer(size.value)
  if not kernel32.QueryFullProcessImageNameW(processHandle, 0, buffer, ctypes.byref(size)):
    raiseLastError("QueryFullProcessImageNameW")
  return buffer.value
