<#
.SYNOPSIS
    Symlinks this project's aircraft .obj files into the ToLiss aircraft folders in
    your X-Plane 12 install, so edits made in this repo apply live in the sim.

.DESCRIPTION
    For each supported aircraft, the X-Plane objects\lights_out3xx_XP12.obj is replaced
    with a symbolic link that points back to this repo's generated copy under dist\.
    After running this, rebuilding (python build\build_objs.py build, or build\watch.py)
    immediately affects X-Plane (reload the aircraft).

    dist\ is build output: run a build at least once before linking, or the link
    targets won't exist.

    Run Unlink-Objects.ps1 to break the connection (it drops a real, static copy in
    place of the link so the aircraft still loads).

    Creating symlinks on Windows requires either Developer Mode enabled
    (Settings > System > For developers) or running this script as Administrator.

.NOTES
    X-Plane root is auto-detected from the repo's Log.txt symlink, then a default Steam
    path. Override with:  $env:XPLANE_ROOT = 'D:\X-Plane 12'  before running.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent

# key -> (project-relative source .obj under dist\, aircraft-folder glob under Aircraft\)
$Aircraft = @(
    @{ Name = 'A319'; Src = 'dist\A319\objects\lights_out319_XP12.obj'; Glob = 'ToLissA319*' }
    @{ Name = 'A320'; Src = 'dist\A320\objects\lights_out320_XP12.obj'; Glob = 'ToLissA320*' }
    @{ Name = 'A321'; Src = 'dist\A321\objects\lights_out321_XP12.obj'; Glob = 'ToLissA321*' }
)

function Get-XPlaneRoot {
    if ($env:XPLANE_ROOT -and (Test-Path $env:XPLANE_ROOT)) {
        return (Resolve-Path $env:XPLANE_ROOT).Path
    }
    $logLink = Join-Path $RepoRoot 'Log.txt'
    if (Test-Path $logLink) {
        $target = (Get-Item $logLink -Force).ResolveLinkTarget($true)
        if ($target) { return (Split-Path $target.FullName -Parent) }
    }
    $fallback = 'C:\Program Files (x86)\Steam\steamapps\common\X-Plane 12'
    if (Test-Path $fallback) { return $fallback }
    throw "Could not locate X-Plane 12. Set `$env:XPLANE_ROOT to your X-Plane 12 folder."
}

$xpRoot = Get-XPlaneRoot
$aircraftDir = Join-Path $xpRoot 'Aircraft'
Write-Host "X-Plane 12: $xpRoot" -ForegroundColor Cyan

foreach ($ac in $Aircraft) {
    $src = Join-Path $RepoRoot $ac.Src
    if (-not (Test-Path $src)) {
        Write-Host "[$($ac.Name)] SKIP - source not found: $src" -ForegroundColor Yellow
        continue
    }
    $srcFull = (Resolve-Path $src).Path
    $objName = Split-Path $src -Leaf

    $matches = @(Get-ChildItem -Path $aircraftDir -Directory -Filter $ac.Glob -ErrorAction SilentlyContinue)
    if ($matches.Count -eq 0) {
        Write-Host "[$($ac.Name)] SKIP - no aircraft folder matching '$($ac.Glob)'" -ForegroundColor Yellow
        continue
    }

    foreach ($folder in $matches) {
        $objDir = Join-Path $folder.FullName 'objects'
        if (-not (Test-Path $objDir)) { New-Item -ItemType Directory -Path $objDir -Force | Out-Null }
        $link = Join-Path $objDir $objName

        if (Test-Path $link) {
            $item = Get-Item $link -Force
            if ($item.LinkType -eq 'SymbolicLink' -and
                $item.ResolveLinkTarget($true) -and
                $item.ResolveLinkTarget($true).FullName -eq $srcFull) {
                Write-Host "[$($ac.Name)] already linked -> $($folder.Name)" -ForegroundColor DarkGray
                continue
            }
            Remove-Item $link -Force
        }

        try {
            New-Item -ItemType SymbolicLink -Path $link -Target $srcFull | Out-Null
            Write-Host "[$($ac.Name)] linked $($folder.Name)\objects\$objName" -ForegroundColor Green
        } catch {
            Write-Host "[$($ac.Name)] FAILED to create symlink: $($_.Exception.Message)" -ForegroundColor Red
            Write-Host "         Enable Developer Mode or run PowerShell as Administrator." -ForegroundColor Red
        }
    }
}

Write-Host "Done. Reload the aircraft in X-Plane to pick up changes." -ForegroundColor Cyan
