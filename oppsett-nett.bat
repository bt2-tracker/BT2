@echo off
rem Engangs-oppsett: legger byggeoppskriften for nettversjonen der GitHub leter etter den.
if not exist "%~dp0.github\workflows" mkdir "%~dp0.github\workflows"
copy /Y "%~dp0verktoy\web.yml" "%~dp0.github\workflows\web.yml" >nul
if errorlevel 1 (echo Noe gikk galt.) else (echo Ferdig! .github\workflows\web.yml er lagt inn. Trykk Commit og Sync.)
