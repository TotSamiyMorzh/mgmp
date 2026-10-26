@echo off
cd /d "%~dp0"
if not exist "game\Mewgenics.exe" (
  echo Run setup.bat first.
  pause
  exit /b 1
)
set "LAST="
if exist host_ip.txt set /p LAST=<host_ip.txt
set "ADDR=%~1"
if "%ADDR%"=="" (
  echo Empty + Enter = join over Steam ^(Steam friends list: "Join Game", or in game F1 - steam - join^).
  set /p "ADDR=Host IP to join over the network [empty = Steam]: "
)
if "%ADDR%"=="" goto steam
>host_ip.txt echo %ADDR%
set "ROLE=client"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$b=[string][char]92; $g=(Resolve-Path 'game/Mewgenics.exe').Path.Replace($b,$b+$b); (Get-Content -Raw -Encoding UTF8 'mgmp.template.json').Replace('@GAME@',$g).Replace('@ROLE@','%ROLE%').Replace('@ADDR@','%ADDR%') | Set-Content -Encoding UTF8 'mgmp.json'"
echo Connecting to %ADDR%:27600 ...
start "" "%~dp0mgmp_loader.exe" "%~dp0game\Mewgenics.exe" -modpaths "%~dp0modpath"
exit /b 0

:steam
rem No address: the session starts when the player joins the host through Steam.
set "ROLE=off"
set "ADDR=127.0.0.1"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$b=[string][char]92; $g=(Resolve-Path 'game/Mewgenics.exe').Path.Replace($b,$b+$b); (Get-Content -Raw -Encoding UTF8 'mgmp.template.json').Replace('@GAME@',$g).Replace('@ROLE@','%ROLE%').Replace('@ADDR@','%ADDR%') | Set-Content -Encoding UTF8 'mgmp.json'"
echo Waiting for a Steam join: in the Steam friends list on the host - "Join Game",
echo or in game F1 - the steam block - join.
start "" "%~dp0mgmp_loader.exe" "%~dp0game\Mewgenics.exe" -modpaths "%~dp0modpath"
