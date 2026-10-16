@echo off
rem Build kk.exe with MSVC. Locates the Visual Studio dev environment, then
rem compiles the single translation unit into a static-CRT self-contained exe.
setlocal
set "VSD=D:\Program Files\Common7\Tools\vsdevcmd.bat"
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
cl /nologo /std:c11 /W4 /O1 /GL /MT /Zi /utf-8 kk.c ^
    /link /LTCG /OPT:REF /OPT:ICF /MAP /OUT:kk.exe /MANIFEST:EMBED /MANIFESTINPUT:kk.manifest ^
    winhttp.lib user32.lib
rem /MAP writes kk.map (RVA -> symbol): kk_crash.txt records RVAs, and kk.map
rem is what names them without a debugger.
rem /O1 over /O2, and the two /OPT flags, are what keep the image small: /Zi
rem implies /DEBUG at link time, /DEBUG switches /OPT:REF and /OPT:ICF off,
rem and ~480 KB of unfolded COMDATs then sit in .text. 840 KB with the flags
rem off, 358 KB with them on, 286 KB once /O1 is added. kk.pdb is unaffected.
