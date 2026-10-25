@echo off
rem Builds TypeThru.exe and Tests.exe into .build\ (or TYPETHRU_BUILD_DIR).
rem Usage: scripts\build.cmd [app|tests|preview|all]
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

if not defined TYPETHRU_BUILD_DIR set TYPETHRU_BUILD_DIR=.build
if not exist "%TYPETHRU_BUILD_DIR%" mkdir "%TYPETHRU_BUILD_DIR%"

set CORE=src\shared.cpp src\keymap.cpp src\typer.cpp src\stopwatcher.cpp
set COMMON=/nologo /std:c++17 /utf-8 /W4 /WX /EHsc /O2 /DUNICODE /D_UNICODE /c /Fo"%TYPETHRU_BUILD_DIR%\\"

if "%TARGET%"=="app"   goto app
if "%TARGET%"=="all"   goto app
if "%TARGET%"=="tests" goto tests
if "%TARGET%"=="preview" goto preview
echo error: unknown target "%TARGET%" ^(app^|tests^|preview^|all^) 1>&2
exit /b 1

:app
rc /nologo /fo "%TYPETHRU_BUILD_DIR%\typethru.res" res\typethru.rc
if errorlevel 1 exit /b 1
cl %COMMON% %CORE% src\main.cpp src\app.cpp src\ui.cpp src\settings.cpp src\hotkey.cpp src\updates.cpp || exit /b 1
link /nologo /out:"%TYPETHRU_BUILD_DIR%\TypeThru.exe" /subsystem:windows /entry:wWinMainCRTStartup "%TYPETHRU_BUILD_DIR%\main.obj" "%TYPETHRU_BUILD_DIR%\app.obj" "%TYPETHRU_BUILD_DIR%\ui.obj" "%TYPETHRU_BUILD_DIR%\settings.obj" "%TYPETHRU_BUILD_DIR%\hotkey.obj" "%TYPETHRU_BUILD_DIR%\updates.obj" "%TYPETHRU_BUILD_DIR%\shared.obj" "%TYPETHRU_BUILD_DIR%\keymap.obj" "%TYPETHRU_BUILD_DIR%\typer.obj" "%TYPETHRU_BUILD_DIR%\stopwatcher.obj" "%TYPETHRU_BUILD_DIR%\typethru.res" user32.lib gdi32.lib gdiplus.lib shell32.lib advapi32.lib comctl32.lib winhttp.lib bcrypt.lib crypt32.lib winmm.lib || exit /b 1
echo built "%TYPETHRU_BUILD_DIR%\TypeThru.exe"
if not "%TARGET%"=="all" exit /b 0

:tests
cl %COMMON% %CORE% tests\tests.cpp src\settings.cpp src\hotkey.cpp src\updates.cpp || exit /b 1
link /nologo /out:"%TYPETHRU_BUILD_DIR%\Tests.exe" "%TYPETHRU_BUILD_DIR%\tests.obj" "%TYPETHRU_BUILD_DIR%\settings.obj" "%TYPETHRU_BUILD_DIR%\hotkey.obj" "%TYPETHRU_BUILD_DIR%\updates.obj" "%TYPETHRU_BUILD_DIR%\shared.obj" "%TYPETHRU_BUILD_DIR%\keymap.obj" "%TYPETHRU_BUILD_DIR%\typer.obj" "%TYPETHRU_BUILD_DIR%\stopwatcher.obj" user32.lib gdi32.lib shell32.lib advapi32.lib comctl32.lib winhttp.lib bcrypt.lib crypt32.lib winmm.lib || exit /b 1
echo built "%TYPETHRU_BUILD_DIR%\Tests.exe"
exit /b 0

:preview
cl %COMMON% src\ui.cpp tools\render-ui.cpp || exit /b 1
link /nologo /out:"%TYPETHRU_BUILD_DIR%\RenderUI.exe" "%TYPETHRU_BUILD_DIR%\render-ui.obj" "%TYPETHRU_BUILD_DIR%\ui.obj" user32.lib gdi32.lib gdiplus.lib comctl32.lib || exit /b 1
echo built "%TYPETHRU_BUILD_DIR%\RenderUI.exe"
exit /b 0
