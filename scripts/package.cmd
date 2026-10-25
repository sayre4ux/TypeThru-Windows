@echo off
rem Builds TypeThru and makes the release files into .build\release\:
rem   TypeThru-<version>-win64.zip      portable copy (the app itself updates from this)
rem   TypeThru-<version>-win64.zip.sig  update signature (needs the TypeThru-Update key)
rem   TypeThru-Setup-<version>.exe      installer, when Inno Setup is installed
rem The version comes from src\version.h.
setlocal enabledelayedexpansion
set TYPETHRU_BUILD_DIR=.build
cd /d "%~dp0.."

for /f "tokens=3" %%v in ('findstr /c:"#define TYPETHRU_VERSION_STRING_A" src\version.h') do set RAW=%%v
set VERSION=%RAW:"=%
if "%VERSION%"=="" (
    echo error: could not read the version from src\version.h 1>&2
    exit /b 1
)
echo packaging version %VERSION%

call scripts\build.cmd all || exit /b 1

set OUT=.build\release
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\TypeThru"
copy /y .build\TypeThru.exe "%OUT%\TypeThru\" >nul

powershell -NoProfile -Command "Compress-Archive -Path '.build\release\TypeThru\*' -DestinationPath '.build\release\TypeThru-%VERSION%-win64.zip' -Force" || exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -File scripts\sign-update.ps1 ".build\release\TypeThru-%VERSION%-win64.zip" || exit /b 1

rem Inno Setup 7 (64-bit and 32-bit editions) and 6 are all supported.
set ISCC=
for %%p in ("%ProgramFiles%\Inno Setup 7\ISCC.exe" "%ProgramFiles(x86)%\Inno Setup 7\ISCC.exe" "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe") do (
    if not defined ISCC if exist %%p set ISCC=%%~p
)
if defined ISCC (
    "%ISCC%" installer\typethru.iss >nul || exit /b 1
    echo built installer TypeThru-Setup-%VERSION%.exe
) else (
    echo note: Inno Setup not found; skipped the installer ^(zip only^)
)

powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $sha=[Security.Cryptography.SHA256]::Create(); $stream=[IO.File]::OpenRead('.build\release\TypeThru-%VERSION%-win64.zip'); try { [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','').ToLowerInvariant() } finally { $stream.Dispose(); $sha.Dispose() }" || exit /b 1

echo done: .build\release\
