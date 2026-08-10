<#
  Build EVERYTHING ToLiss Photon puts in X-Plane 12, and deploy it (Windows).

  This is the one command. A change to the C++, to the .phdsl lighting, or to the
  overlay recipes all reach the sim the same way:

      powershell -ExecutionPolicy Bypass -File src\native\deploy.ps1

  or just double-click deploy.bat. Then start X-Plane. Nothing else is needed --
  if a session tells you to run build_objs.py by hand afterwards, that session is
  working from a stale recipe.

  What it does, in order:
    1. (-Test only) the unit suite -- python -m unittest discover -s tests
    2. the overlay starter images   -- build/make_overlays.py
    3. the aircraft OBJs, into the live install
                                    -- build/build_objs.py build --target all --write
    4. the plugin                   -- cmake configure + build
    5. copies the .xpl + overlays into X-Plane, and links panelfx.txt back here

  Options:
    -XPlaneRoot <path>   X-Plane 12 folder (default: the Steam install). Exported
                         as XPLANE_ROOT so the OBJ build targets the SAME sim --
                         build_objs.py locates X-Plane on its own otherwise.
    -Dev                 build with the DEV TOOLING in: the Plugins > ToLiss
                         Photon > Dev menu and its window (profiles, panel probe,
                         FX compositor + history, repo build commands, log).
                         The probe can paint over the displays - never ship it.
                         Re-run without -Dev to get a clean plugin back.
                         It does NOT imply a --debug OBJ build: those bind the
                         ToLissPhoton/debug/light/* pool and are not installable,
                         so the light tuner stays an explicit, separate step:
    -DebugObjs           build the DEBUG OBJ variant: the interior and screens
                         OBJs re-emitted with every light live-tunable from the
                         Dev window's Lights tab (the exterior stays a normal
                         build - it has frozen goldens). Adds per-light dataref
                         overhead, so leave it off when assessing performance.
                         Usually paired with -Dev; without a dev plugin the
                         debug lights draw at seed colors and ignore every knob.
                         Re-run without -DebugObjs to put the real OBJs back.
    -Test                run the unit suite first and stop if it fails (~10 s).
    -NoObjs              skip steps 2-3; plugin only.
    -NoBuild             skip step 4; just copy the already-built .xpl.
    -Config <cfg>        build config (default: Release)
    -Target <t>          what to hand build_objs.py (default: all = exterior +
                         interior + screens). `both` omits the screens OBJ.
    -Wing <w>            wing-mod variant: stock (default), durantula, realwings.
                         Must match what is actually installed -- a stock build
                         over a RealWings install leaves the wingtips unpatched.

  The two plugin builds go to SEPARATE folders (build/ and build-dev/), so
  switching between them is a copy rather than a full rebuild.

  Notes: X-Plane must be CLOSED (a loaded .xpl is locked and can't be
  overwritten), and that is checked BEFORE any work so a refusal costs nothing.
  Windows/win_x64 only.
#>
[CmdletBinding()]
param(
    [string] $XPlaneRoot = "C:\Program Files (x86)\Steam\steamapps\common\X-Plane 12",
    [switch] $NoBuild,
    [switch] $NoObjs,
    [switch] $Test,
    [string] $Config = "Release",
    [ValidateSet('exterior','interior','screens','both','all')]
    [string] $Target = "all",
    [ValidateSet('stock','durantula','realwings')]
    [string] $Wing = "stock",
    [switch] $Dev,
    [switch] $DebugObjs
)

$ErrorActionPreference = 'Stop'
$here  = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo  = Split-Path -Parent (Split-Path -Parent $here)   # src/native -> src -> repo
# Separate build trees per flavor. One tree would mean a full recompile every
# time you flip -Dev, which is the friction this whole pass is about removing.
$build = Join-Path $here $(if ($Dev) { 'build-dev' } else { 'build' })
$xpl   = Join-Path $build 'ToLissPhoton\win_x64\ToLissPhoton.xpl'

function Find-CMake {
    $c = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if ($c) { return $c }
    $known = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $known) { return $known }
    $found = Get-ChildItem "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match 'CMake\\CMake\\bin' } | Select-Object -First 1
    if ($found) { return $found.FullName }
    throw "cmake not found on PATH or in Visual Studio. Install CMake, or add it to PATH."
}

# `py` is the fallback because a Store-alias `python.exe` on PATH is a stub that
# opens the Microsoft Store and exits 9009 -- it resolves, so Get-Command alone
# would pick it and every python step would "fail" with no output.
function Find-Python {
    foreach ($cand in @(@{ Exe = 'python'; Pre = @() }, @{ Exe = 'py'; Pre = @('-3') })) {
        $src = (Get-Command $cand.Exe -ErrorAction SilentlyContinue).Source
        if (-not $src) { continue }
        & $src @($cand.Pre) -c "import sys" *> $null
        if ($LASTEXITCODE -eq 0) { return @{ Exe = $src; Pre = $cand.Pre } }
    }
    throw "python not found on PATH (tried 'python' and 'py -3'). Install Python 3, or pass -NoObjs to build the plugin alone."
}

# Run a repo script from the repo root, and stop the deploy if it fails. A step
# that failed but let the rest continue is the exact failure this script exists
# to remove: the .xpl lands, the OBJs don't, and the sim looks half-updated.
# NOTE: the parameter is $PyArgs, not $Args -- $Args is an automatic variable
# inside every function, and a parameter of that name is silently overwritten by
# the caller's unbound arguments, i.e. by nothing. python then gets no arguments
# at all, opens a REPL on a closed stdin and exits 1 with no output.
function Invoke-RepoPython {
    param([hashtable] $Py, [string[]] $PyArgs, [string] $What)
    Write-Host $What -ForegroundColor Cyan
    Push-Location $repo
    try {
        # -u: unbuffered, so python's progress interleaves with Write-Host in the
        # right order when the deploy is piped to a log rather than a console.
        & $Py.Exe @($Py.Pre) -u @PyArgs
        if ($LASTEXITCODE -ne 0) { throw "$What failed (exit $LASTEXITCODE)" }
    } finally { Pop-Location }
}

# Checked FIRST, before cmake and before anything is written. These used to sit
# after the build, so a run with the sim open spent a full compile to reach a
# refusal it could have made in a millisecond.
if (-not (Test-Path (Join-Path $XPlaneRoot 'Resources\plugins'))) {
    throw "not an X-Plane 12 folder (no Resources\plugins): $XPlaneRoot  (pass -XPlaneRoot)"
}
if (Get-Process -Name 'X-Plane' -ErrorAction SilentlyContinue) {
    throw "X-Plane is running. Close it fully, then re-run - a loaded .xpl is locked and cannot be overwritten."
}
# build_objs.py --write finds X-Plane by its own rules ($XPLANE_ROOT -> the
# repo's Log.txt symlink -> the Steam default). Exporting -XPlaneRoot is what
# stops a deploy from putting the .xpl in one install and the OBJs in another.
$env:XPLANE_ROOT = $XPlaneRoot

if ($Test -or -not $NoObjs) {
    $py = Find-Python
    if ($Test) {
        Invoke-RepoPython $py @('-m','unittest','discover','-s','tests') "Running tests..."
    }
    if (-not $NoObjs) {
        # Before the copy below, which seeds these into the plugin folder.
        Invoke-RepoPython $py @('build/make_overlays.py') "Generating overlay images..."
        # --write installs each OBJ into the live aircraft as well as dist/.
        # Default target is `all`: `both` silently omits the screens OBJ, which
        # is what makes an edited screens fixture look like it did not rebuild.
        # --debug only applies to the debug-capable targets (interior, screens);
        # cmd_build scopes it, so the exterior stays a normal build either way.
        $objArgs = @('build/build_objs.py','build','--target',$Target,'--wing',$Wing,'--write')
        if ($DebugObjs) {
            $objArgs += '--debug'
            Write-Host "DEBUG OBJs - interior + screens lights are live-tunable. Not installable, never ship dist/ from this state." -ForegroundColor Yellow
        }
        Invoke-RepoPython $py $objArgs `
            "Building OBJs ($Target, $Wing$(if ($DebugObjs) { ', DEBUG' })) into $XPlaneRoot..."
    }
}

if (-not $NoBuild) {
    $cmake = Find-CMake
    # Always configure, and always state PHOTON_DEV explicitly. A cached ON would
    # otherwise be sticky: every later plain deploy would keep shipping a plugin
    # that can paint magenta over the captain's PFD.
    $devArg = if ($Dev) { "-DPHOTON_DEV=ON" } else { "-DPHOTON_DEV=OFF" }
    if ($Dev) {
        Write-Host "DEV BUILD - Dev menu and panel probe are IN. Do not ship." -ForegroundColor Yellow
    }
    Write-Host "Configuring..." -ForegroundColor Cyan
    & $cmake -S $here -B $build -A x64 $devArg
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (exit $LASTEXITCODE)" }
    Write-Host "Building $Config..." -ForegroundColor Cyan
    & $cmake --build $build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed (exit $LASTEXITCODE)" }
}

if (-not (Test-Path $xpl)) {
    throw "built plugin not found: $xpl  (build it first, or drop -NoBuild)"
}

$dst = Join-Path $XPlaneRoot 'Resources\plugins\ToLissPhoton\win_x64\ToLissPhoton.xpl'
New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
Copy-Item $xpl $dst -Force
$info = Get-Item $dst
Write-Host ("deployed -> {0}" -f $dst) -ForegroundColor Green
Write-Host ("           {0:N0} bytes, {1}" -f $info.Length, $info.LastWriteTime)

# Starter overlay images for the Panel FX compositor. Copied by NAME, never as a
# folder mirror: the same folder is where you drop your own images, and a
# Copy-Item of the whole tree with -Force is one typo away from deleting them.
# Regenerate the generated ones with `python build/make_overlays.py`.
$overlaySrc = Join-Path $here 'overlays'
if (Test-Path $overlaySrc) {
    $overlayDst = Join-Path $XPlaneRoot 'Resources\plugins\ToLissPhoton\overlays'
    New-Item -ItemType Directory -Force -Path $overlayDst | Out-Null
    $n = 0
    Get-ChildItem $overlaySrc -File | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $overlayDst $_.Name) -Force
        $n++
    }
    Write-Host ("           {0} starter overlay image(s) -> {1}" -f $n, $overlayDst)
}

# The Panel FX layer definition, as a SYMLINK back into the repo.
#
# The plugin reads panelfx.txt out of its own folder, and a -Dev build WRITES it
# whenever you commit an edit in the FX editor. Linking it means an in-sim tuning
# pass lands directly in source control, with no export step to forget - which is
# the whole reason the file moved out of Output/preferences. The installer knows
# to skip a symlinked copy, so running install.py against this X-Plane does not
# quietly break the loop.
#
# A copy is the fallback, and it is a real downgrade rather than an equivalent:
# edits then go to a file `git status` will never mention. Symlinks need Developer
# Mode or an elevated shell on Windows.
$fxSrc = Join-Path $here 'panelfx.txt'
if (Test-Path $fxSrc) {
    $fxDst = Join-Path $XPlaneRoot 'Resources\plugins\ToLissPhoton\panelfx.txt'
    $existing = Get-Item $fxDst -Force -ErrorAction SilentlyContinue
    if ($existing -and $existing.LinkType -eq 'SymbolicLink') {
        Write-Host ("           panelfx.txt already linked -> {0}" -f $existing.Target)
    } else {
        if ($existing) { Remove-Item $fxDst -Force }
        try {
            New-Item -ItemType SymbolicLink -Path $fxDst -Target $fxSrc -Force | Out-Null
            Write-Host ("           panelfx.txt LINKED -> {0}" -f $fxSrc) -ForegroundColor Green
        } catch {
            Copy-Item $fxSrc $fxDst -Force
            Write-Host ("           panelfx.txt COPIED (symlink refused: {0})" -f $_.Exception.Message) -ForegroundColor Yellow
            # ⚠ Almost always this shell, not this machine. Windows PowerShell 5.1
            # never passes SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE, so it
            # demands elevation even with Developer Mode on; pwsh 7 does and
            # succeeds. deploy.bat prefers pwsh for exactly this.
            Write-Host "           In-sim FX edits will NOT reach the repo. Re-run under pwsh (PowerShell 7), or elevated." -ForegroundColor Yellow
        }
    }
}

if ($Dev) {
    Write-Host "           DEV build - Plugins > ToLiss Photon > Dev > Dev window..." -ForegroundColor Yellow
    # Seed the Dev window's repo path, ONCE. Without it the Build tab has nowhere
    # to run commands and -- the reason this is here -- the light tuner writes
    # light_tuning.txt beside X-Plane's preferences instead of into .scratch/,
    # which is a file the next person looking for their tuning will not find.
    # Never overwritten: after the first write it is the user's to edit in the UI.
    $devPrefs = Join-Path $XPlaneRoot 'Output\preferences\ToLissPhoton_dev.txt'
    if (-not (Test-Path $devPrefs)) {
        New-Item -ItemType Directory -Force -Path (Split-Path $devPrefs) | Out-Null
        Set-Content -Path $devPrefs -Value "repo $repo" -Encoding utf8
        Write-Host ("           dev repo path seeded -> {0}" -f $devPrefs)
    }
}

# One summary, so "did the OBJs actually go in?" is answerable without scrolling
# back through cmake's output.
Write-Host ""
Write-Host "Deployed:" -ForegroundColor Green
Write-Host ("  plugin   {0} build -> {1}" -f $(if ($Dev) { 'DEV' } else { 'release' }), $XPlaneRoot)
if ($NoObjs) {
    Write-Host "  OBJs     SKIPPED (-NoObjs) - the aircraft lights in the sim are whatever was there before"
} else {
    Write-Host ("  OBJs     {0} ({1} wings{2}) -> the installed aircraft" -f $Target, $Wing, $(if ($DebugObjs) { ', DEBUG' } else { '' }))
    if ($DebugObjs) {
        Write-Host "  note     DEBUG OBJs installed: tune from Dev > Lights (screens edit as one) and" -ForegroundColor Yellow
        Write-Host "           Dev > Cockpit (simplified-flood multiplier); Save tuning writes" -ForegroundColor Yellow
        Write-Host "           .scratch\light_tuning.txt. Re-deploy without -DebugObjs when done." -ForegroundColor Yellow
    }
}
if (-not $Test) {
    Write-Host "  tests    not run (pass -Test)"
}
# The screens OBJ is inert until the .acf attaches it, and a ToLiss update
# reverts that attachment -- so a rebuilt lights_screens.obj that draws nothing
# is a normal, recurring state, not a bug in the OBJ.
if ($Target -in @('all','screens')) {
    Write-Host "  note     lights_screens.obj only draws once the .acf attaches it. That is part of" -ForegroundColor DarkGray
    Write-Host "           install.py's base install; re-run it (or build/patch_acf_screens.py) after" -ForegroundColor DarkGray
    Write-Host "           a ToLiss update, which reverts the attachment." -ForegroundColor DarkGray
}
Write-Host "Start X-Plane." -ForegroundColor Green
