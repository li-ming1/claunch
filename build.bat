@echo off
rem Build claunch.exe with MSVC. Locates the Visual Studio dev environment, then
rem compiles the translation unit (plus one asm shim) into a self-contained exe
rem that links NO C runtime at all.  (Keep comments ASCII: cmd.exe line parsing
rem chokes on non-ASCII bytes in .bat files -- it split a rem line and silently
rem skipped every following command.)
setlocal
set "VSD=D:/Program Files\Common7\Tools\vsdevcmd.bat"
if not exist "%VSD%" set "VSD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\vsdevcmd.bat"
rem last resort: whatever vswhere reports as the newest toolset install (CI
rem images ship a different edition than the hardcoded paths above)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSD%" if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSD=%%i\Common7\Tools\vsdevcmd.bat"
if not exist "%VSD%" (
    echo build.bat: vsdevcmd.bat not found; edit VSD in this script.
    exit /b 1
)
call "%VSD%" -arch=x64 -no_logo
if errorlevel 1 exit /b 1
rem claunch.asm provides two compiler shims that only ucrt would otherwise offer:
rem   __chkstk        stack probe (size in RAX), makes the guard page grow safely
rem   __GSHandlerCheck referenced by /GS on the exception path
ml64 /nologo /c /Fo:claunch_chkstk.obj claunch.asm
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O1 /GS /GL /Zi /utf-8 /D CLAUNCH_NOCRT claunch.c ^
    /link /NODEFAULTLIB /ENTRY:wWinMainCRTStartup /SUBSYSTEM:WINDOWS ^
    /LTCG /OPT:REF /OPT:ICF /MAP /OUT:claunch.exe /MANIFEST:EMBED /MANIFESTINPUT:claunch.manifest ^
    claunch_chkstk.obj vcruntime.lib kernel32.lib user32.lib shell32.lib gdi32.lib imm32.lib advapi32.lib ole32.lib
if errorlevel 1 exit /b 1
rem Why this is leaner than the classic static-CRT build:
rem   /NODEFAULTLIB + the explicit import list above   -> libcmt/ucrt gone.
rem       An empty wWinMain program already costs 102KB of CRT floor; with all of
rem       the above, the whole launcher is 72KB instead of 174KB.
rem   /D CLAUNCH_NOCRT                            -> homegrown entry point,
rem       bootstrapped /GS security cookie, and own wide-char primitives
rem       (they used to drag in the whole locale/nls data chain).
rem   vcruntime.lib only                        -> compiler support lib
rem       (memcpy/memmove/memset/memcmp), no CRT startup.
rem   comdlg32 is now lazy-loaded by hand (GetOpenFileNameW), so /DELAYLOAD and
rem       delayimp are gone and comdlg32 disappears from the import table.
rem /MAP writes claunch.map (RVA -> symbol): claunch_crash.txt records RVAs, and
rem claunch.map is what names them without a debugger.
