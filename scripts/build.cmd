@echo off
rem Builds TypeThru.exe (default), TypeThruPOC.exe, and Tests.exe into .build\.
rem Usage: scripts\build.cmd [app|poc|tests|all]
setlocal enabledelayedexpansion
cd /d "%~dp0.."
set TARGET=%1
if "%TARGET%"=="" set TARGET=all

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo error: vswhere not found. Install Visual Studio 2022 with the C++ tools. 1>&2
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSROOT=%%i
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo error: MSVC x64 tools not found. 1>&2
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist .build mkdir .build

set CORE=src\shared.cpp src\keymap.cpp src\typer.cpp src\stopwatcher.cpp
set COMMON=/nologo /std:c++17 /W4 /EHsc /O2 /DUNICODE /D_UNICODE /c /Fo.build\

if "%TARGET%"=="app"   goto app
if "%TARGET%"=="all"   goto app
if "%TARGET%"=="poc"   goto poc
if "%TARGET%"=="tests" goto tests
echo error: unknown target "%TARGET%" ^(app^|poc^|tests^|all^) 1>&2
exit /b 1

:app
rc /nologo /fo .build\typethru.res res\typethru.rc
if errorlevel 1 exit /b 1
cl %COMMON% %CORE% src\main.cpp src\app.cpp src\settings.cpp src\hotkey.cpp src\updates.cpp || exit /b 1
link /nologo /out:.build\TypeThru.exe /subsystem:windows /entry:wWinMainCRTStartup .build\main.obj .build\app.obj .build\settings.obj .build\hotkey.obj .build\updates.obj .build\shared.obj .build\keymap.obj .build\typer.obj .build\stopwatcher.obj .build\typethru.res user32.lib gdi32.lib shell32.lib advapi32.lib comctl32.lib winhttp.lib bcrypt.lib crypt32.lib winmm.lib || exit /b 1
echo built .build\TypeThru.exe
if not "%TARGET%"=="all" exit /b 0

:poc
cl %COMMON% %CORE% tools\poc.cpp || exit /b 1
link /nologo /out:.build\TypeThruPOC.exe .build\poc.obj .build\shared.obj .build\keymap.obj .build\typer.obj .build\stopwatcher.obj user32.lib gdi32.lib shell32.lib winmm.lib || exit /b 1
echo built .build\TypeThruPOC.exe
if not "%TARGET%"=="all" exit /b 0

:tests
cl %COMMON% %CORE% tests\tests.cpp src\settings.cpp src\hotkey.cpp src\updates.cpp || exit /b 1
link /nologo /out:.build\Tests.exe .build\tests.obj .build\settings.obj .build\hotkey.obj .build\updates.obj .build\shared.obj .build\keymap.obj .build\typer.obj .build\stopwatcher.obj user32.lib gdi32.lib shell32.lib advapi32.lib comctl32.lib winhttp.lib bcrypt.lib crypt32.lib winmm.lib || exit /b 1
echo built .build\Tests.exe
exit /b 0
