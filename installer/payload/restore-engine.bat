@echo off
rem Puts back the engine files the installer moved into the "engine-removed" folder.
if not exist "%~dp0engine-removed" (
  echo Nothing to restore.
  pause
  exit /b 0
)
xcopy /E /I /Y /Q "%~dp0engine-removed" "%~dp0engine"
echo.
echo Files restored. If you want the original program name back, open ieb.ini in this folder
echo and change  EngineExe=ieengine.exe  to  EngineExe=firefox.exe
pause
