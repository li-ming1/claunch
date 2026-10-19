@echo off
rem In-sandbox build script: mirrors the cl/ml64/link lines of build.bat exactly.
rem ASCII only -- cmd.exe mangles non-ASCII bytes in .bat files.
set "VCT=D:/Program Files\VC\Tools\MSVC\14.51.36231"
set "SDK=D:/Windows Kits\10"
set "SDKV=10.0.22621.0"
set "PATH=%VCT%\bin\Hostx64\x64;%SDK%\bin\%SDKV%\x64;%PATH%"
set "INCLUDE=%VCT%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\winrt;%SDK%\Include\%SDKV%\cppwinrt"
set "LIB=%VCT%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"
ml64 /nologo /c /Fo claunch_chkstk.obj claunch.asm
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O1 /GS /GL /Zi /utf-8 /D CLAUNCH_NOCRT claunch.c ^
    /link /NODEFAULTLIB /ENTRY:wWinMainCRTStartup /SUBSYSTEM:WINDOWS ^
    /LTCG /OPT:REF /OPT:ICF /MAP /OUT:claunch.exe /MANIFEST:EMBED /MANIFESTINPUT:claunch.manifest ^
    claunch_chkstk.obj kernel32.lib user32.lib shell32.lib gdi32.lib imm32.lib advapi32.lib ole32.lib
