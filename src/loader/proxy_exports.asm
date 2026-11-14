; Forwarding stubs for the 17 functions exported by the system version.dll.
;
; Each stub jumps straight to the real function, whose address proxy.cpp stores in
; g_realVersionFn when the DLL is loaded. Arguments and return value pass through untouched,
; so the game sees exactly the behaviour of the Windows DLL.

EXTERN g_realVersionFn:QWORD

.code

FORWARD MACRO name, index
proxy_&name PROC
    jmp QWORD PTR [g_realVersionFn + index * 8]
proxy_&name ENDP
ENDM

FORWARD GetFileVersionInfoA,        0
FORWARD GetFileVersionInfoByHandle, 1
FORWARD GetFileVersionInfoExA,      2
FORWARD GetFileVersionInfoExW,      3
FORWARD GetFileVersionInfoSizeA,    4
FORWARD GetFileVersionInfoSizeExA,  5
FORWARD GetFileVersionInfoSizeExW,  6
FORWARD GetFileVersionInfoSizeW,    7
FORWARD GetFileVersionInfoW,        8
FORWARD VerFindFileA,               9
FORWARD VerFindFileW,               10
FORWARD VerInstallFileA,            11
FORWARD VerInstallFileW,            12
FORWARD VerLanguageNameA,           13
FORWARD VerLanguageNameW,           14
FORWARD VerQueryValueA,             15
FORWARD VerQueryValueW,             16

END
