@echo off
setlocal
set "F=%~1"
findstr /v /b /i "%~2=" "%F%" > "%F%.tmp"
(echo %~2=%~3)>> "%F%.tmp"
move /y "%F%.tmp" "%F%" >nul
