<#
.SYNOPSIS
    Breaks the symlink connection created by Link-Objects.ps1, replacing each link with
    a real (static) copy of the current project .obj so the aircraft still loads.

.DESCRIPTION
    For each supported aircraft, if the X-Plane objects\lights_out3xx_XP12.obj is a
    symlink into this repo, it is removed and a plain copy of the project file is written
    in its place. After this, edits in the project NO LONGER affect X-Plane until you run
    Link-Objects.ps1 again -- useful for testing the add-on as a normal (copied) install.

    A file that is already a real (non-link) copy is left untouched.

.NOTES
    X-Plane root is auto-detected from the repo's Log.txt symlink, then a default Steam
    path. Override with:  $env:XPLANE_ROOT = 'D:\X-Plane 12'  before running.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent

# key -> (project-relative source .obj, aircraft-folder glob under Aircraft\)
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
    $objName = Split-Path $ac.Src -Leaf

    $matches = @(Get-ChildItem -Path $aircraftDir -Directory -Filter $ac.Glob -ErrorAction SilentlyContinue)
    if ($matches.Count -eq 0) {
        Write-Host "[$($ac.Name)] SKIP - no aircraft folder matching '$($ac.Glob)'" -ForegroundColor Yellow
        continue
    }

    foreach ($folder in $matches) {
        $objDir = Join-Path $folder.FullName 'objects'
        $link = Join-Path $objDir $objName

        if (-not (Test-Path $link)) {
            # Nothing there: drop a copy in so the aircraft still has its object.
            if (Test-Path $src) {
                New-Item -ItemType Directory -Path $objDir -Force | Out-Null
                Copy-Item $src $link -Force
                Write-Host "[$($ac.Name)] copied fresh file -> $($folder.Name)" -ForegroundColor Green
            } else {
                Write-Host "[$($ac.Name)] SKIP - no link and no source to copy" -ForegroundColor Yellow
            }
            continue
        }

        $item = Get-Item $link -Force
        if ($item.LinkType -eq 'SymbolicLink') {
            if (-not (Test-Path $src)) {
                Write-Host "[$($ac.Name)] SKIP - link present but project source missing; leaving link intact" -ForegroundColor Yellow
                continue
            }
            Remove-Item $link -Force
            Copy-Item $src $link -Force
            Write-Host "[$($ac.Name)] unlinked $($folder.Name) (now a static copy)" -ForegroundColor Green
        } else {
            Write-Host "[$($ac.Name)] already a real file -> $($folder.Name) (left as is)" -ForegroundColor DarkGray
        }
    }
}

Write-Host "Done. X-Plane is now using static copies (edits in the repo will not apply)." -ForegroundColor Cyan
