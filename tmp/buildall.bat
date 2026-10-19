@echo off
rem Builds every probe in this directory with one common flag set.
rem ASCII only -- cmd.exe mangles non-ASCII bytes in .bat files.
rem   tmp\buildall.bat            build all probes
rem   tmp\buildall.bat kbtest     build just one (name without .c)
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "VCT=D:/Program Files\VC\Tools\MSVC\14.51.36231"
set "SDK=D:/Windows Kits\10"
set "SDKV=10.0.22621.0"
set "PATH=%VCT%\bin\Hostx64\x64;%SDK%\bin\%SDKV%\x64;%PATH%"
set "INCLUDE=%VCT%\include;%SDK%\Include\%SDKV%\ucrt;%SDK%\Include\%SDKV%\um;%SDK%\Include\%SDKV%\shared;%SDK%\Include\%SDKV%\winrt;%SDK%\Include\%SDKV%\cppwinrt"
set "LIB=%VCT%\lib\x64;%SDK%\Lib\%SDKV%\ucrt\x64;%SDK%\Lib\%SDKV%\um\x64"
set "failed="
if not "%~1"=="" goto one
for %%f in (*.c) do (
    cl /nologo /std:c11 /W4 /O1 /MT /utf-8 "%%f" ^
        /Fe:"%%~nf.exe" user32.lib kernel32.lib gdi32.lib shell32.lib ole32.lib psapi.lib advapi32.lib
    if errorlevel 1 set "failed=!failed! %%~nf.c"
)
if defined failed echo FAILED:!failed!
echo buildall done
exit /b 0
:one
cl /nologo /std:c11 /W4 /O1 /MT /utf-8 "%~1.c" ^
    /Fe:"%~1.exe" user32.lib kernel32.lib gdi32.lib shell32.lib ole32.lib psapi.lib advapi32.lib
if errorlevel 1 echo "%1" FAILED
exit /b 0
