@echo off
setlocal

if defined JIELI_TOOL_DIR goto tool_dir_configured
set "JIELI_RANLIB_HAS_TOOL_DIR="
for /f "delims==" %%A in ('set JIELI_TOOL_DIR 2^>nul') do set "JIELI_RANLIB_HAS_TOOL_DIR=1"
if defined JIELI_RANLIB_HAS_TOOL_DIR (
    echo JIELI_TOOL_DIR is set but empty 1>&2
    exit /b 1
)

set "JIELI_TOOL_DIR=%~dp0.tools\portable-jieli-windows\pi32\bin"
if exist "%JIELI_TOOL_DIR%\lto-ar.exe" goto tool_dir_configured
set "JIELI_TOOL_DIR=C:\JL\pi32\bin"

:tool_dir_configured
if not exist "%JIELI_TOOL_DIR%\lto-ar.exe" (
    echo Jieli lto-ar not found in "%JIELI_TOOL_DIR%" 1>&2
    exit /b 1
)

"%JIELI_TOOL_DIR%\lto-ar.exe" s %*
exit /b %ERRORLEVEL%
