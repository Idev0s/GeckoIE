@echo off
setlocal
set "BASE=%~1"
if "%BASE%"=="" set "BASE=%~dp0.."
set "REGASM=%windir%\Microsoft.NET\Framework\v2.0.50727\regasm.exe"
if not exist "%REGASM%" set "REGASM=%windir%\Microsoft.NET\Framework\v4.0.30319\regasm.exe"
if not exist "%BASE%\bho\IEBridgeBho.dll" exit /b 0
"%REGASM%" /unregister "%BASE%\bho\IEBridgeBho.dll"
exit /b 0
