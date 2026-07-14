@echo off
setlocal
rem Applies the PowerManager "Power relays" segment-card patch to a WLED tree's index.js.
rem Usage: double-click (auto-discovers WLED trees next to this file), or:
rem        Apply-PowerManager-UI-Patch.bat <path-to-wled-tree> [--dry-run]

set "SCRIPT_DIR=%~dp0"
set "PYEXE=python"
where python >nul 2>nul
if errorlevel 1 set "PYEXE=py"

"%PYEXE%" "%SCRIPT_DIR%apply_powermanager_ui_patch.py" %*
set "RC=%ERRORLEVEL%"

echo.
pause
exit /b %RC%
