# pvp_soak.ps1 -- unattended PvP soak test: two copies fight each other with the
# game's AI driving both sides (pvp.ini [auto]), round after round, and every
# desync, crash or stall is collected and the copies restarted.
#
#   .\tools\pvp_soak.ps1 [-ModPaths <dir>] -StartSave <backup dir> [-Hours 4]
#
# -StartSave is a backup folder holding <steam id>\saves\ (a copy of the game's
# save folder, taken in the house). It is copied over the live save folder before
# every launch, so each session starts from the same house.
#
# Per incident a folder under soak\<timestamp>_<reason>\ gets both peers' logs:
# mgmp_*.log (newest), pvp_arena.log, pvp_auto.log, pvp_facewatch.log.
# soak\summary.txt has one line per session.
param(
    [string]$ModPaths = "",
    [Parameter(Mandatory)] [string]$StartSave,
    [double]$Hours       = 4,
    # Empty = the newest numbered profile folder under %APPDATA%\Glaiel Games\Mewgenics.
    [string]$SteamId     = '',
    # A session with no new line in the host's pvp_auto.log for this long is stuck.
    [int]   $StallMinutes = 15
)

$ErrorActionPreference = 'Stop'
$root    = Split-Path $PSScriptRoot -Parent
$netTest = Join-Path $PSScriptRoot 'net_test.ps1'
if (-not $SteamId) {
    $SteamId = (Get-ChildItem (Join-Path $env:APPDATA 'Glaiel Games\Mewgenics') -Directory | Where-Object Name -match '^\d+$' | Sort-Object LastWriteTime | Select-Object -Last 1).Name
}
$saves   = Join-Path $env:APPDATA "Glaiel Games\Mewgenics\$SteamId\saves"
$srcSave = Join-Path $StartSave "$SteamId\saves"
$soakDir = Join-Path $root 'soak'
$hostDir = Join-Path $root 'build_host'
$cliDir  = Join-Path $root 'build_client'
New-Item -ItemType Directory -Force $soakDir | Out-Null
if (-not (Test-Path (Join-Path $srcSave 'steamcampaign01.sav'))) { throw "no steamcampaign01.sav under $srcSave" }

function Game-Count { @(Get-Process -Name Mewgenics -ErrorAction SilentlyContinue).Count }

function Stop-All {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $netTest -Stop | Out-Null
    for ($i = 0; $i -lt 30 -and (Game-Count) -gt 0; $i++) { Start-Sleep -Seconds 1 }
    Get-Process -Name Mewgenics -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

function Restore-Save {
    Copy-Item (Join-Path $srcSave '*') $saves -Recurse -Force
    Get-ChildItem $saves -Filter 'steamcampaign01_2026-*.savbackup' -ErrorAction SilentlyContinue | Remove-Item -Force
}

function Newest($dir, $pattern) {
    Get-ChildItem $dir -Filter $pattern -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
}

function Fight-Count($dir) { Line-Count $dir 'fight \d+ over' }

# Lines in a peer's pvp_auto.log matching a pattern (FileShare-friendly read).
function Line-Count($dir, $pattern) {
    $f = Join-Path $dir 'pvp_auto.log'
    if (-not (Test-Path $f)) { return 0 }
    try {
        $fs = [System.IO.File]::Open($f, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd(); $sr.Close()
    } catch { return 0 }
    @($text -split "`n" | Where-Object { $_ -match $pattern }).Count
}

function Collect($reason) {
    $ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    $dst = Join-Path $soakDir "${ts}_$reason"
    New-Item -ItemType Directory -Force $dst | Out-Null
    foreach ($p in @(@{ d = $hostDir; n = 'host' }, @{ d = $cliDir; n = 'client' })) {
        $log = Newest $p.d "mgmp_$($p.n)_*.log"
        if ($log) { Copy-Item $log.FullName (Join-Path $dst $log.Name) }
        foreach ($f in 'pvp_arena.log', 'pvp_auto.log', 'pvp_facewatch.log', 'mgmp_boot.log') {
            $src = Join-Path $p.d $f
            if (Test-Path $src) { Copy-Item $src (Join-Path $dst "$($p.n)_$f") }
        }
    }
    return $dst
}

# Once, before anything is overwritten: the live save folder as it was.
$bk = Join-Path $soakDir ("saves_before_soak_" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if ((Game-Count) -gt 0) { throw "Mewgenics is running -- close it first (the game rewrites the save on exit)" }
Copy-Item (Split-Path $saves -Parent) $bk -Recurse
Write-Host "==> live saves backed up to $bk" -ForegroundColor DarkGray

# Fresh per-soak auto logs, so fight counts are per soak.
foreach ($d in $hostDir, $cliDir) { Remove-Item (Join-Path $d 'pvp_auto.log') -ErrorAction SilentlyContinue }

$deadline = (Get-Date).AddHours($Hours)
$session = 0
$totalFights = 0
while ((Get-Date) -lt $deadline) {
    $session++
    Stop-All
    Restore-Save
    Write-Host "==> session $session" -ForegroundColor Cyan
    $cliHalts0 = Line-Count $cliDir 'HALT in fight'
    $mp = @(); if ($ModPaths) { $mp = @('-ModPaths', $ModPaths) }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $netTest -Pvp -Auto @mp | Out-Null
    $started = Get-Date
    $fights0 = Fight-Count $hostDir
    # Judged by CONTENT, never by LastWriteTime: Windows does not refresh a file's
    # timestamp while the game holds it open, so a halted log looked untouched.
    $halts0  = Line-Count $hostDir 'HALT in fight'
    $lines0  = Line-Count $hostDir '.'
    $lastLines = $lines0
    $lastChange = Get-Date
    $reason = $null
    while (-not $reason) {
        Start-Sleep -Seconds 15
        if ((Get-Date) -ge $deadline) { $reason = 'deadline'; break }
        if (((Get-Date) - $started).TotalSeconds -gt 60 -and (Game-Count) -lt 2) { $reason = 'crash'; break }
        $hostHalted = (Line-Count $hostDir 'HALT in fight') -gt $halts0
        $cliHalted  = (Line-Count $cliDir 'HALT in fight') -gt $cliHalts0
        if ($hostHalted -or $cliHalted) { Start-Sleep -Seconds 5; $reason = 'desync'; break }
        $n = Line-Count $hostDir '.'
        if ($n -ne $lastLines) { $lastLines = $n; $lastChange = Get-Date }
        elseif (((Get-Date) - $lastChange).TotalMinutes -gt $StallMinutes) { $reason = 'stall' }
    }
    $fights = (Fight-Count $hostDir) - $fights0
    $totalFights += $fights
    $where = if ($reason -ne 'deadline') { Collect $reason } else { '-' }
    $line = "{0}  session {1}: {2} after {3:N0} min, {4} fight(s) finished -> {5}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm'), $session, $reason, ((Get-Date) - $started).TotalMinutes, $fights, $where
    Add-Content -Path (Join-Path $soakDir 'summary.txt') -Value $line
    Write-Host $line -ForegroundColor $(if ($reason -eq 'deadline') { 'Green' } else { 'Yellow' })
}
Stop-All
Restore-Save
Write-Host "==> soak done: $session session(s), $totalFights fight(s). See $soakDir\summary.txt" -ForegroundColor Green
