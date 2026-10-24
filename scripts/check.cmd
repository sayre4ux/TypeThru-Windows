@echo off
rem Everything that must pass before a push: tests, then a clean full build.
cd /d "%~dp0.."
call scripts\test.cmd || exit /b 1
if exist .build rmdir /s /q .build
call scripts\build.cmd all || exit /b 1
echo check passed
