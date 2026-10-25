@echo off
rem Removes the engine startup caches so a repacked/changed engine is read fresh.
for /d %%d in ("%APPDATA%\GeckoIE\profile-inproc*") do if exist "%%d\startupCache" rd /s /q "%%d\startupCache"
for /d %%d in ("%APPDATA%\IEBridge\profile-inproc*") do if exist "%%d\startupCache" rd /s /q "%%d\startupCache"
