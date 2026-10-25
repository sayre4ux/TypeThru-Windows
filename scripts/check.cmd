@echo off
rem Everything that must pass before a push: tests, then a clean full build.
setlocal
cd /d "%~dp0.."
rem Keep running binaries, release artifacts, and signing backups untouched.
set TYPETHRU_BUILD_DIR=.build\check
if not exist .build\check mkdir .build\check
del /q .build\check\*.obj .build\check\*.res .build\check\*.exe 2>nul
call scripts\build.cmd all || exit /b 1
.build\check\Tests.exe || exit /b 1
echo check passed
