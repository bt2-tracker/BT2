@echo off
rem Bygger BerntTracker og starter programmet. Kjor: .\bygg (i VS Code-terminalen)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verktoy\bygg.ps1" -Start
if errorlevel 1 pause
