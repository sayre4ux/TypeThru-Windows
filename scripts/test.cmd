@echo off
rem Runs the unit tests; posts no input.
cd /d "%~dp0.."
call scripts\build.cmd tests || exit /b 1
.build\Tests.exe
