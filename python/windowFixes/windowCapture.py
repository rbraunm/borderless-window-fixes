"""Captures the game's window without bringing it forward."""
import ctypes
from ctypes import wintypes

from PIL import Image

from windowFixes.windowsApi import raiseLastError, user32

gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
printWindowRenderFullContent = 2
perMonitorAwareV2 = ctypes.c_void_p(-4)
for function, argumentTypes in ((user32.GetWindowDC, [wintypes.HWND]), (user32.ReleaseDC, [wintypes.HWND, wintypes.HDC]),
    (user32.PrintWindow, [wintypes.HWND, wintypes.HDC, wintypes.UINT]), (gdi32.CreateCompatibleDC, [wintypes.HDC]),
    (gdi32.CreateCompatibleBitmap, [wintypes.HDC, ctypes.c_int, ctypes.c_int]), (gdi32.SelectObject, [wintypes.HDC, wintypes.HGDIOBJ]),
    (gdi32.DeleteObject, [wintypes.HGDIOBJ]), (gdi32.DeleteDC, [wintypes.HDC])):
  function.argtypes = argumentTypes
user32.GetWindowDC.restype = wintypes.HDC
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
user32.SetThreadDpiAwarenessContext.restype = ctypes.c_void_p
user32.SetThreadDpiAwarenessContext.argtypes = [ctypes.c_void_p]


def captureWindow(window):
  # Physical pixels, whatever the display scaling.
  previousAwareness = user32.SetThreadDpiAwarenessContext(perMonitorAwareV2)
  if not previousAwareness:
    raiseLastError("SetThreadDpiAwarenessContext")
  try:
    return captureWindowPixels(window)
  finally:
    user32.SetThreadDpiAwarenessContext(previousAwareness)


def captureWindowPixels(window):
  rectangle = wintypes.RECT()
  if not user32.GetWindowRect(window, ctypes.byref(rectangle)):
    raiseLastError("GetWindowRect")
  width, height = rectangle.right - rectangle.left, rectangle.bottom - rectangle.top
  windowContext = user32.GetWindowDC(window)
  memoryContext = gdi32.CreateCompatibleDC(windowContext)
  bitmap = gdi32.CreateCompatibleBitmap(windowContext, width, height)
  try:
    gdi32.SelectObject(memoryContext, bitmap)
    if not user32.PrintWindow(window, memoryContext, printWindowRenderFullContent):
      raiseLastError("PrintWindow")
    pixels = ctypes.create_string_buffer(width * height * 4)
    header = (ctypes.c_uint32 * 10)(40, width, (-height) & 0xFFFFFFFF, 1 | (32 << 16), 0, 0, 0, 0, 0, 0)
    if gdi32.GetDIBits(memoryContext, bitmap, 0, height, pixels, header, 0) != height:
      raiseLastError("GetDIBits")
  finally:
    gdi32.DeleteObject(bitmap)
    gdi32.DeleteDC(memoryContext)
    user32.ReleaseDC(window, windowContext)
  return Image.frombuffer("RGBA", (width, height), pixels, "raw", "BGRA", 0, 1).convert("RGB")

