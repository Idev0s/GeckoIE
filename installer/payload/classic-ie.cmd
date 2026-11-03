@echo off
rem Opens Internet Explorer with its own engine only (no IEBridge engine) for this window.
set IEBRIDGE_CLASSIC=1
start "" "%ProgramFiles%\Internet Explorer\iexplore.exe" %*
