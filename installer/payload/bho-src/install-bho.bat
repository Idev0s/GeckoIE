@echo off
setlocal
set "BASE=%~1"
if "%BASE%"=="" set "BASE=%~dp0.."
set "FW2=%windir%\Microsoft.NET\Framework\v2.0.50727"
set "FW35=%windir%\Microsoft.NET\Framework\v3.5"
set "FW4=%windir%\Microsoft.NET\Framework\v4.0.30319"
set "CSC="
set "REGASM="
if exist "%FW2%\csc.exe" if exist "%FW2%\regasm.exe" (
  set "CSC=%FW2%\csc.exe"
  set "REGASM=%FW2%\regasm.exe"
)
if not defined CSC if exist "%FW35%\csc.exe" if exist "%FW2%\regasm.exe" (
  set "CSC=%FW35%\csc.exe"
  set "REGASM=%FW2%\regasm.exe"
)
if not defined CSC if exist "%FW4%\csc.exe" if exist "%FW4%\regasm.exe" (
  set "CSC=%FW4%\csc.exe"
  set "REGASM=%FW4%\regasm.exe"
)
if not defined CSC (
  echo ERROR: no .NET Framework 2.0 or newer found.
  exit /b 2
)
echo Using %CSC%
if not exist "%BASE%\bho" mkdir "%BASE%\bho"
"%CSC%" /nologo /target:library /platform:x86 /out:"%BASE%\bho\IEBridgeBho.dll" /r:System.dll /r:System.Windows.Forms.dll "%BASE%\bho-src\*.cs"
if errorlevel 1 (
  echo ERROR: could not build the Internet Explorer add-on.
  exit /b 3
)
"%REGASM%" /codebase "%BASE%\bho\IEBridgeBho.dll"
if errorlevel 1 (
  echo ERROR: could not register the Internet Explorer add-on.
  exit /b 4
)
for %%I in ("%REGASM%") do set "NGEN=%%~dpIngen.exe"
if exist "%NGEN%" "%NGEN%" install "%BASE%\bho\IEBridgeBho.dll" >nul 2>&1
echo Internet Explorer add-on installed.
exit /b 0
