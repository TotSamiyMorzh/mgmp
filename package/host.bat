@echo off
cd /d "%~dp0"
if not exist "game\Mewgenics.exe" (
  echo Run setup.bat first.
  pause
  exit /b 1
)
set "ROLE=host"
set "ADDR=0.0.0.0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$b=[string][char]92; $g=(Resolve-Path 'game/Mewgenics.exe').Path.Replace($b,$b+$b); (Get-Content -Raw -Encoding UTF8 'mgmp.template.json').Replace('@GAME@',$g).Replace('@ROLE@','%ROLE%').Replace('@ADDR@','%ADDR%') | Set-Content -Encoding UTF8 'mgmp.json'"
echo Starting the host: over Steam (friends use "Join Game") and on TCP port 27600.
start "" "%~dp0mgmp_loader.exe" "%~dp0game\Mewgenics.exe" -modpaths "%~dp0modpath"
