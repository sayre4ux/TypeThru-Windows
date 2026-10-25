@echo off
rem Runs the unit tests; posts no input.
setlocal
if not defined TYPETHRU_BUILD_DIR set TYPETHRU_BUILD_DIR=.build
cd /d "%~dp0.."
call scripts\build.cmd tests || exit /b 1
"%TYPETHRU_BUILD_DIR%\Tests.exe"
