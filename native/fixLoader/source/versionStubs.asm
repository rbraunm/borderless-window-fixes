; Every export of the system version.dll, reproduced as a single jump through the pointer
; versionProxy.cpp resolves (realX), so the caller's arguments and return address reach the
; real function untouched. MSVC has no inline assembly for x64, hence this file.

EXTERN realGetFileVersionInfoA:QWORD
EXTERN realGetFileVersionInfoByHandle:QWORD
EXTERN realGetFileVersionInfoExA:QWORD
EXTERN realGetFileVersionInfoExW:QWORD
EXTERN realGetFileVersionInfoSizeA:QWORD
EXTERN realGetFileVersionInfoSizeExA:QWORD
EXTERN realGetFileVersionInfoSizeExW:QWORD
EXTERN realGetFileVersionInfoSizeW:QWORD
EXTERN realGetFileVersionInfoW:QWORD
EXTERN realVerFindFileA:QWORD
EXTERN realVerFindFileW:QWORD
EXTERN realVerInstallFileA:QWORD
EXTERN realVerInstallFileW:QWORD
EXTERN realVerLanguageNameA:QWORD
EXTERN realVerLanguageNameW:QWORD
EXTERN realVerQueryValueA:QWORD
EXTERN realVerQueryValueW:QWORD

.CODE

proxyGetFileVersionInfoA PROC
  jmp QWORD PTR [realGetFileVersionInfoA]
proxyGetFileVersionInfoA ENDP

proxyGetFileVersionInfoByHandle PROC
  jmp QWORD PTR [realGetFileVersionInfoByHandle]
proxyGetFileVersionInfoByHandle ENDP

proxyGetFileVersionInfoExA PROC
  jmp QWORD PTR [realGetFileVersionInfoExA]
proxyGetFileVersionInfoExA ENDP

proxyGetFileVersionInfoExW PROC
  jmp QWORD PTR [realGetFileVersionInfoExW]
proxyGetFileVersionInfoExW ENDP

proxyGetFileVersionInfoSizeA PROC
  jmp QWORD PTR [realGetFileVersionInfoSizeA]
proxyGetFileVersionInfoSizeA ENDP

proxyGetFileVersionInfoSizeExA PROC
  jmp QWORD PTR [realGetFileVersionInfoSizeExA]
proxyGetFileVersionInfoSizeExA ENDP

proxyGetFileVersionInfoSizeExW PROC
  jmp QWORD PTR [realGetFileVersionInfoSizeExW]
proxyGetFileVersionInfoSizeExW ENDP

proxyGetFileVersionInfoSizeW PROC
  jmp QWORD PTR [realGetFileVersionInfoSizeW]
proxyGetFileVersionInfoSizeW ENDP

proxyGetFileVersionInfoW PROC
  jmp QWORD PTR [realGetFileVersionInfoW]
proxyGetFileVersionInfoW ENDP

proxyVerFindFileA PROC
  jmp QWORD PTR [realVerFindFileA]
proxyVerFindFileA ENDP

proxyVerFindFileW PROC
  jmp QWORD PTR [realVerFindFileW]
proxyVerFindFileW ENDP

proxyVerInstallFileA PROC
  jmp QWORD PTR [realVerInstallFileA]
proxyVerInstallFileA ENDP

proxyVerInstallFileW PROC
  jmp QWORD PTR [realVerInstallFileW]
proxyVerInstallFileW ENDP

proxyVerLanguageNameA PROC
  jmp QWORD PTR [realVerLanguageNameA]
proxyVerLanguageNameA ENDP

proxyVerLanguageNameW PROC
  jmp QWORD PTR [realVerLanguageNameW]
proxyVerLanguageNameW ENDP

proxyVerQueryValueA PROC
  jmp QWORD PTR [realVerQueryValueA]
proxyVerQueryValueA ENDP

proxyVerQueryValueW PROC
  jmp QWORD PTR [realVerQueryValueW]
proxyVerQueryValueW ENDP

END
