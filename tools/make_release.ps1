# make_release.ps1 -- builds the PvP release zip from package\ and the Release build.
#
#   .\tools\make_release.ps1 -Version 1.0.0
#
# Output: dist\mewgenics_pvp_<Version>.zip, laid out as it unpacks:
#   mewgenics_pvp\  mgmp.dll  mgmp_loader.exe  *.bat  README.txt  CHANGELOG.txt
#                   pvp.ini  mgmp.template.json  licenses\
# Nothing from the game goes in: the loader and the DLL read the player's own
# resources.gpak at run time. The script refuses a zip that contains any file
# type the game ships (.gon .lvl .swf .png .csv .gpak .sav) as a last check.
param(
    [Parameter(Mandatory)] [string]$Version,
    [string]$BuildDir = 'build',
    [string]$Config   = 'Release'
)
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$out   = Join-Path $root "$BuildDir\$Config"
$stage = Join-Path $root 'dist\release_stage'
$pkg   = Join-Path $stage 'mewgenics_pvp'
$zip   = Join-Path $root "dist\mewgenics_pvp_$Version.zip"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $pkg 'licenses') | Out-Null

foreach ($f in 'mgmp.dll', 'mgmp_loader.exe') {
    $src = Join-Path $out $f
    if (-not (Test-Path $src)) { throw "missing $src -- build $Config first" }
    Copy-Item $src $pkg
}
Copy-Item (Join-Path $root 'package\*') $pkg
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $pkg 'licenses\mgmp_LICENSE.txt')
Copy-Item (Join-Path $root 'third_party\minhook\LICENSE.txt') (Join-Path $pkg 'licenses\minhook_LICENSE.txt')
Copy-Item (Join-Path $root 'third_party\imgui\LICENSE.txt') (Join-Path $pkg 'licenses\imgui_LICENSE.txt')

$bad = Get-ChildItem $pkg -Recurse -File | Where-Object { $_.Extension -in '.gon', '.lvl', '.swf', '.png', '.csv', '.gpak', '.sav' }
if ($bad) { throw "game-type files in the package: $($bad.Name -join ', ')" }

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $pkg -DestinationPath $zip
Write-Host "==> $zip" -ForegroundColor Green
Get-ChildItem $pkg -Recurse -File | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.FullName.Substring($pkg.Length + 1) }
