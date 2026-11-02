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
    [Parameter(Mandatory)] [ValidatePattern('^\d+\.\d+(\.\d+)?$')] [string]$Version,
    [string]$BuildDir = 'build',
    [string]$Config   = 'Release'
)
$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$out   = Join-Path $root "$BuildDir\$Config"
$stage = Join-Path $root 'dist\release_stage'
$pkg   = Join-Path $stage 'mewgenics_pvp'
$zip   = Join-Path $root "dist\mewgenics_pvp_$Version.zip"

$distRoot = [IO.Path]::GetFullPath((Join-Path $root 'dist')) + [IO.Path]::DirectorySeparatorChar
if (-not [IO.Path]::GetFullPath($stage).StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'release stage outside dist' }
if (-not [IO.Path]::GetFullPath($zip).StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'release zip outside dist' }
$cache = Join-Path $root "$BuildDir\CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cache) -or
    -not (Select-String -LiteralPath $cache -Pattern '^MGMP_SYNCTEST:BOOL=OFF$' -Quiet)) {
    throw 'Player releases require a verified MGMP_SYNCTEST=OFF build'
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
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
Copy-Item (Join-Path $root 'third_party\json_LICENSE.txt') (Join-Path $pkg 'licenses\json_LICENSE.txt')
Copy-Item (Join-Path $root 'third_party\stb_LICENSE.txt') (Join-Path $pkg 'licenses\stb_LICENSE.txt')
"Mewgenics PvP $Version`r`nGame: 1.1.21239`r`nProtocol: 29`r`nSynthetic stress harness: OFF" | Set-Content (Join-Path $pkg 'VERSION.txt') -Encoding ASCII

$bad = Get-ChildItem $pkg -Recurse -File | Where-Object { $_.Extension -in '.gon', '.lvl', '.swf', '.png', '.csv', '.gpak', '.sav' }
if ($bad) { throw "game-type files in the package: $($bad.Name -join ', ')" }

if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path $pkg -DestinationPath $zip
$digest = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
"$digest  $([IO.Path]::GetFileName($zip))" | Set-Content ($zip + '.sha256') -Encoding ASCII
Write-Host "==> $zip" -ForegroundColor Green
Get-ChildItem $pkg -Recurse -File | ForEach-Object { '{0,10}  {1}' -f $_.Length, $_.FullName.Substring($pkg.Length + 1) }
