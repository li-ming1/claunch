@echo off
rem Build claunch.exe with MSVC. Locates the Visual Studio dev environment, then
rem compiles the single translation unit (plus one asm shim) into a self-contained
rem exe that links **no C runtime at all**.
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
rem claunch.asm 提供不链 CRT 时的两个编译器桩：__chkstk（栈探针）与
rem __GSHandlerCheck（/GS 异常路径引用点）。ucrt 才有正式实现
ml64 /nologo /c /Fo:claunch_chkstk.obj claunch.asm
if errorlevel 1 exit /b 1
cl /nologo /std:c11 /W4 /O1 /GS /GL /Zi /utf-8 /D CLAUNCH_NOCRT claunch.c ^
    /link /NODEFAULTLIB /ENTRY:wWinMainCRTStartup /SUBSYSTEM:WINDOWS ^
    /LTCG /OPT:REF /OPT:ICF /MAP /OUT:claunch.exe /MANIFEST:EMBED /MANIFESTINPUT:claunch.manifest ^
    claunch_chkstk.obj vcruntime.lib kernel32.lib user32.lib shell32.lib gdi32.lib imm32.lib advapi32.lib ole32.lib
rem /NODEFAULTLIB + 上面显式列导入库：静态 CRT 被彻底拿掉——一个空 wWinMain 程序
rem 的地板是 102KB，本程序加上这些东西只有 72KB。/D CLAUNCH_NOCRT 带来入口自举、
rem /GS cookie 自举与 chkstk.asm。
rem /MAP writes claunch.map (RVA -> symbol): claunch_crash.txt records RVAs, and
rem claunch.map is what names them without a debugger.
