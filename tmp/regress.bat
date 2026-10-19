@echo off
rem Isolated regression entry: copies claunch.exe and the seed config into tmp\reg\
rem and runs every probe against that copy only -- never the user's live claunch.ini.
rem   tmp\regress.bat          full suite
rem   tmp\regress.bat kbtest   one probe only
rem ASCII only -- cmd.exe mangles non-ASCII bytes in .bat files (learned twice).
setlocal
cd /d "%~dp0"
if not exist reg (mkdir reg) else (del /q reg\*.* 2>nul)
copy /y ..\claunch.exe reg\c.exe >nul
if errorlevel 1 echo "FAIL: build ..\claunch.exe first" & exit /b 1
copy /y seed.ini reg\claunch.ini >nul
if errorlevel 1 echo "FAIL: seed fixture tmp\seed.ini missing" & exit /b 1
rem Clear stale instances. killours only: taskkill /IM is case-insensitive and
rem would kill the user's concurrently running CLaunch.exe (same name, different
rem program). It matches window class + full image path.
killours reg\c.exe
if "%~1"=="" goto all
call :run %~1 %2 %3
goto end
:all
call :run kbtest
call :run soak 4
call :run starttime 3
call :run dragwake
call :run dropchk
call :run probe2
call :run parsefuzz 20000
goto end
:run
echo === %~1 ===
if "%~1"=="parsefuzz" (parsefuzz.exe %2) else (%~1.exe reg\c.exe %2 %3)
if errorlevel 1 echo "!!! %~1 FAILED"
exit /b 0
:end
del /q reg\*.* 2>nul
rmdir reg
echo regress done
