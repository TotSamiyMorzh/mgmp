@echo off
cd /d "%~dp0"
rem Makes game\ next to this script: the Steam copy's three files as hard links
rem (no 5 GB copy, nothing in the Steam folder changes) plus steam_appid.txt, so
rem the game starts here instead of being relaunched by Steam without the mod.
rem Usage: setup.bat "D:\SteamLibrary\steamapps\common\Mewgenics"
set "STEAMGAME=%~1"
if "%STEAMGAME%"=="" set /p "STEAMGAME=Mewgenics folder in Steam (the one with Mewgenics.exe): "
set "STEAMGAME=%STEAMGAME:"=%"
if not exist "%STEAMGAME%\Mewgenics.exe" (
  echo Mewgenics.exe was not found in "%STEAMGAME%"
  pause
  exit /b 1
)
if not exist game mkdir game
for %%F in (Mewgenics.exe resources.gpak steam_api64.dll) do (
  if exist "game\%%F" del /q "game\%%F"
  mklink /H "game\%%F" "%STEAMGAME%\%%F" >nul 2>&1 || (
    echo Could not hard-link %%F ^(another drive?^) - copying instead...
    copy /y "%STEAMGAME%\%%F" "game\%%F" >nul
  )
)
>game\steam_appid.txt echo 686060
echo.
echo Done: the game copy is in "%~dp0game"
pause
