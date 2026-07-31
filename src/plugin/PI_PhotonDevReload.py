"""
PI_PhotonDevReload.py  -  XPPython3 DEV plugin for the ToLiss Photon Lighting project

  *** DEVELOPER TOOL - NOT shipped by the installer. ***
  make_release.py / installer/payload.py reference the runtime plugin by its exact name
  (PI_ToLissPhoton.py), so this second PI_*.py in src/plugin/ is never bundled. To use it,
  copy it into <X-Plane>/Resources/plugins/PythonPlugins/ next to PI_ToLissPhoton.py and
  delete the stale __pycache__ there (same manual step the main plugin needs).

WHAT IT AUTOMATES
  Iterating on exterior-light POSITIONS means editing the OBJ, then repeating a tedious
  in-sim dance every time. This plugin collapses the whole dance into ONE bindable command
  (bind a key or joystick button - ideal while the camera is parked outside):

     1. Snapshot the current EXTERNAL CAMERA pose.
     2. Snapshot a configurable list of DATAREFS (light switches + external power) so the
        aircraft's post-reload cold state can be put back exactly.
     3. Disable the aircraft's own plugins - the same crash-avoidance step you do by hand in
        Plugin Admin before reloading an OBJ. Scoped by FILE PATH (every plugin whose binary
        lives under the current aircraft's folder), minus SkipPluginNameMatches. NEVER match
        by name - a name match on "sasl" also catches unrelated scenery plugins and hard-
        crashes the sim. See docs/dev-tools.md.
     4. Reload the aircraft (sim/operation/reload_aircraft) - re-reads the edited OBJ. Issued
        straight from the command handler: reloading from a flight loop hard-crashes the sim.
     5. When the reloaded plane reports loaded (MSG_PLANE_LOADED), SETTLE: re-enable the
        plugins, then WAIT OUT ToLiss's cold-start init rather than racing it - watch the
        snapshot datarefs until ToLiss has visibly touched them and then gone quiet, and only
        then write the snapshot back (power back on, your light switches back on).
     6. Restore the CAMERA to the exact pose you were looking from and HOLD it there,
        re-grabbing if ToLiss steals it, until you give input (any key / click / wheel).

WHY A COMMAND (not just a dataref/menu): commands are bindable to keys and joystick
buttons, so you can fire the whole cycle without moving the mouse to a menu while framing
a shot outside. A "Photon Dev" Plugins menu is also provided as a mouse fallback.

ALSO IN HERE (all reachable from the Plugins > Photon Dev menu)

  PLAIN RELOAD - the reload with none of the above. Still disables/re-enables the
  aircraft's plugins (that is the crash avoidance, not a convenience), but takes no
  snapshot and never touches the camera. Use it when you just want the OBJ re-read and
  don't mind landing back in a cold cockpit.

  CINEMATIC CAMERA - a slow, eased camera for recording pans. X-Plane exposes no
  free-camera speed dataref, and its own *_slow command variants are still far too fast,
  so this drives the camera itself and INTERCEPTS the sim's view commands
  (sim/general/forward, rot_left, ...) while active - meaning your existing keys, hat
  switch and joystick fly it, at a speed you set, with velocity easing in and out.

  COCKPIT LIGHT TUNER - find a light's position/aim/cone/size in-sim. OBJ light geometry
  is read once at aircraft load, so edits are staged in memory, written into the
  installed lights_inn.obj in one batch, and picked up by a quick reload (which restores
  the camera, so every iteration frames the identical shot). The OUTPUT is the "Log DSL"
  line: paste it into src/lights/lights.layout.phdsl, which stays the source of truth.
  The OBJ the tuner writes is build output and the next build overwrites it.

TUNE IN-SIM: the timing constants and (especially) the external-power dataref name below
are the parts to confirm in the sim. Every snapshot dataref is logged with found/value at
trigger time, so tailing XPPython3Log.txt tells you immediately whether a name resolves.
"""
import json
import math
import os
import time
import traceback
from XPPython3 import xp


# ============================== TUNABLES =============================================
# Datarefs to snapshot before the reload and re-assert afterwards. Type is auto-detected
# (scalar int/float/double AND int/float arrays all work), so just list names. Whatever
# they read at trigger time is written back during the settle window - if your light
# switches and external power were on, they come back on.
#
#   AirbusFBW/OHPLightSwitches  - the whole exterior-light switch array (all lights).
#
# EXTERNAL POWER: add the ToLiss dataref/switch that turns on external (ground) power here.
# The exact name varies by ToLiss version - uncomment/edit one of the candidates below and
# check XPPython3Log.txt after a trigger: a "MISSING" line means the name is wrong. If ext
# power is a *command* rather than a writable dataref, put it in RestoreCommands instead.
SnapshotDataRefs = (
    "AirbusFBW/OHPLightSwitches",       # all exterior-light switches (int array)
    "AirbusFBW/ElecOHPArray[3]",        # external (ground) power pushbutton: 1/0 (array idx 3)
)

# Commands to run once during the settle window (for anything that's a command, not a
# writable dataref - e.g. an external-power pushbutton toggle). Left empty by default.
RestoreCommands = (
    # "AirbusFBW/EnableExternalPower",
)

# Which plugins to disable before the reload (and re-enable after). We disable exactly the
# plugins that live inside the CURRENT AIRCRAFT's folder - i.e. ToLiss's own "AirbusFBW -
# XP11" and "A319/A321 (SASL)" - matched by FILE PATH, not by name. Do NOT match on the
# name "SASL": SASL is a shared scripting engine used by many unrelated add-ons (scenery,
# other aircraft), and disabling e.g. "NIMBUS_AIRPORTS (SASL)" mid-session tears down its
# live draw callbacks and hard-crashes the sim. Path-scoping structurally can't touch any
# global/scenery plugin. (Optional extra guard: names here further restrict WHICH aircraft-
# folder plugins get disabled; empty = disable all of them, which matches the manual step.)
DisablePluginNameMatches = ()   # empty = every plugin under the aircraft folder

# EXCLUSIONS (applied on top of the path scope): aircraft-folder plugins whose name matches
# any of these substrings (lowercased) are NEVER disabled. ToLiss ships some third-party
# plugins inside the aircraft folder that hard-crash if disabled/reloaded mid-session (rain
# effects, etc.) - keep them running. Add more substrings here if others turn out fragile.
SkipPluginNameMatches = ("librain", "kosp")

ReloadCommand   = "sim/operation/reload_aircraft"   # the "Reload the current aircraft" cmd
FreeCameraCommand = "sim/view/free_camera"          # external free-look view for restore
ViewTypeDataRef = "sim/graphics/view/view_type"     # current view mode (logged for diagnostics)

# Timing (seconds). The restore WAITS OUT ToLiss's own post-load init instead of racing it.
# ToLiss resets the switches/view during a cold-start init that runs AFTER MSG_PLANE_LOADED,
# so we watch the snapshot datarefs, wait until we've SEEN ToLiss change them and then go
# quiet (stable for QuietSeconds = init finished), and only then write our snapshot back. If
# ToLiss never visibly touches them, we restore anyway after ActivityTimeoutSeconds; a hard
# MaxSettleSeconds bounds the whole wait.
RestoreTickSeconds      = 0.1   # how often we poll the datarefs during the settle wait
QuietSeconds            = 1.5   # snapshot datarefs stable this long => ToLiss done init'ing
ActivityTimeoutSeconds  = 10.0  # if NO ToLiss activity is seen by now, restore anyway
RestoreGuardSeconds     = 2.0   # after the restore write, keep re-asserting this long
MaxSettleSeconds        = 45.0  # hard cap on the settle wait. Must exceed ToLiss's slowest
                                # init - if we cap out BEFORE ToLiss quiesces we restore while
                                # it's still initialising, and it clobbers power AND the camera
                                # view mode (which culls the exterior). Raise if you still see
                                # "max-cap" in the log instead of "quiesced".
# Camera restore: after ToLiss's init has quiesced we position the saved EXTERNAL pose and
# HOLD it, re-asserting every frame so ToLiss's later view resets can't steal it back. Two
# distinct steal mechanisms, two defences: (a) ToLiss moves the camera while we still OWN it
# -> our CameraFunc overwrites the position every frame, so we win; (b) ToLiss takes camera
# OWNERSHIP (its own controlCamera / a view-mode change), which fires inIsLosingControl and
# STOPS CameraFunc from being called at all - the per-frame overwrite then dies and ToLiss
# wins permanently. (b) is what made the hold "ultimately revert to whatever ToLiss set":
# losing control used to tear the hold down. The fix is CameraWatchLoop, a watchdog that
# notices we've lost ownership and RE-GRABS it (re-enter free view, then controlCamera a
# frame later), so the hold genuinely persists through ToLiss's late steals. It ends ONLY on
# the user's own keyboard/mouse input - key press, or mouse click / right-click / scroll wheel
# (passive cursor MOVEMENT does not count; the triggering input passes straight through so you
# seamlessly grab the free camera) - or the manual "Release Camera" command/menu. This is only
# safe because the settle above waits for true quiescence first; holding while ToLiss was still
# resetting the view mode is what previously culled the exterior ("objects not visible").
MaxCameraHoldSeconds    = 0.0   # 0 = hold INDEFINITELY (until user input / Release Camera).
                                # A positive value is an optional safety cap: release after
                                # that many seconds even with no input.
CameraRegrabInterval    = 0.1   # how often the watchdog polls for "did something take the
                                # camera from us?" so it can re-grab. Cheap boolean check.

MaxArrayElements = 1024        # cap when snapshotting array datarefs (OHPLightSwitches is
                               # tiny; this only bounds pathological sizes)

# ---- cinematic camera ---------------------------------------------------------------
# A slow, smoothed camera you fly with YOUR EXISTING view keybindings. X-Plane has no
# dataref for free-camera speed (checked: nothing in DataRefs.txt), and its own _slow
# command variants are still far too fast for a panning shot - so instead of trying to
# scale the sim's motion we take the camera and drive it ourselves.
#
# HOW THE INPUT WORKS: we intercept the sim's own view commands (sim/general/left,
# forward, rot_left, zoom_in, ... and their _fast/_slow variants) with BEFORE-phase
# handlers that consume them while cinematic mode is on. So whatever you already have
# bound - keys, hat switch, joystick - flies the cinematic camera at OUR speed, with no
# new bindings to learn. Turn the mode off and the bindings go straight back to the sim.
#
# The smoothing is what makes it cinematic: velocity EASES toward the target instead of
# snapping, so starts and stops are soft rather than stepped. CineSmoothingSeconds is the
# time constant - larger = longer, more languid ramps (0 = instant, mechanical).
CineMoveSpeed      = 0.35   # metres/second at multiplier 1.0 (deliberately very slow)
CineRotateSpeed    = 3.0    # degrees/second at multiplier 1.0
CineZoomSpeed      = 0.25   # zoom factor change per second at multiplier 1.0
CineSmoothingSeconds = 0.6  # velocity ease-in/out time constant (0 = no smoothing)
CineFastScale      = 4.0    # the sim's *_fast command variants
CineSlowScale      = 0.25   # the sim's *_slow command variants
CineMultipliers    = (0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0)   # the -/+ buttons step these
CineDefaultMultiplierIndex = 3   # -> 1.0

# ---- cockpit light tuner ------------------------------------------------------------
# Position/aim/cone/size of an OBJ light are BAKED INTO THE OBJ - X-Plane reads them at
# aircraft load, and no dataref can move them afterwards. So the tuner edits the installed
# lights_inn.obj in place and then runs the quick reload (which puts your camera back
# exactly where it was, so you see the same view with the light moved).
#
# ⚠ The OBJ IS BUILD OUTPUT. This tool is for FINDING the numbers in-sim, never for
# keeping them: once a value looks right, hit "Log DSL" and paste it into
# src/lights/lights.layout.phdsl, which stays the single source of truth. The next
# `build_objs.py build --write` overwrites whatever the tuner wrote.
TunerObjRelPath = os.path.join("objects", "lights_inn.obj")
TunerFineStep   = {"pos": 0.01, "dir": 0.02, "size": 0.05, "cone": 0.005}
TunerCoarseStep = {"pos": 0.10, "dir": 0.10, "size": 0.25, "cone": 0.020}

# ---- live light debug ---------------------------------------------------------------
# The other half of the tuner above, which can only move geometry and only by rewriting
# the OBJ and reloading. Build the cockpit OBJ with
#
#     python build/build_objs.py build --target interior --debug --write
#
# and every light becomes a LIGHT_SPILL_CUSTOM bound to ToLissPhoton/debug/light/<n>,
# with a lights_inn.debug.json manifest beside it. We register those datarefs here and
# feed them from the Live Light Debug window, so color, aim, cone, size and brightness
# are live knobs with no rebuild and no reload.
#
# Mainly an IDENTIFICATION tool: Isolate (black out all but the selected light) and Mark
# (paint it magenta) answer "which lamp is that?" in seconds.
#
# ⚠ A debug OBJ has NO category gating, so the Cockpit profile menu does nothing while it
# is installed. Put the real one back with `build --target interior --write`.
# One manifest per debug-capable build target, newest-first at load time. Both
# number their slots from 0, so only ONE debug OBJ can be driven at a time; we take
# the most recently WRITTEN manifest, which is the one you just built, and name it
# in the window so there is never a question which is live. Keep in step with
# DEBUG_MANIFESTS in build/build_objs.py.
DebugManifests = (
    ("cockpit", os.path.join("objects", "lights_inn.debug.json")),
    ("screens", os.path.join("objects", "lights_screens.debug.json")),
)
DebugManifestRelPath = DebugManifests[0][1]   # back-compat alias
DebugDataRefPrefix   = "ToLissPhoton/debug/light/"
# Position comes by a different route: the debug build wraps each light in three
# dataref-driven ANIM_trans blocks reading DebugPosDataRef[3n+axis] as an offset in
# metres. One shared array — a dataref per axis per light would be 192 registrations.
# DebugPosRange is the ANIM keyframe span and must equal DEBUG_POS_RANGE in
# build/build_objs.py; a mismatch silently rescales every position edit. It is far wider
# than a tuning nudge needs because the orientation cycler below drives this array too,
# by as much as the coordinates themselves (cockpit z runs to about -5 m).
DebugPosDataRef = "ToLissPhoton/debug/pos"
DebugPosRange   = 12.0
# A/B toggle over the two lines the debug OBJ emits per light: 0 = untouched original,
# 1 = tunable spill copy. The only way to tell a bad LIGHT_SPILL_CUSTOM conversion apart
# from bad authored values — they look identical in the cockpit.
DebugCompareDataRef = "ToLissPhoton/debug/compare"
# Must match DEBUG_MAX_LIGHTS in build/build_objs.py. Registered at XPluginStart, before
# any OBJ loads: an OBJ binds dataref names at LOAD time and a name that does not exist
# yet reads 0 forever, leaving every debug light black.
DebugMaxLights  = 64
DebugParamCount = 9                       # r g b a size dx dy dz cone
# ⚠ THE DATAREF ORDER IS NOT THE OBJ ORDER. Measured in-sim 2026-07-30 with the slot
# probe below, on the screen glow:
#
#   the OBJ8 LINE is   ... <size> <dx> <dy> <dz> <semi> <dataref>
#   the DATAREF wants  r g b a size   SEMI dx dy dz
#
# i.e. the cone comes FIRST in the trailing group of four, not last. Proof both ways:
# every real `LIGHT_SPILL_CUSTOM` line in the sim's own OBJs has a UNIT-length vector
# when read as `dx dy dz semi` and a 1.26-length one when read as `semi dx dy dz`, so
# the literal order is settled; and probing our slots 5/6/7/8 one at a time gave
# omni / right / up / aft, which is exactly SEMI/DX/DY/DZ.
#
# Consequences worth keeping straight:
#   * ALPHA IS STILL INDEX 3 in both orderings, so plugin.cpp's kSpillAlphaSlot is
#     correct and the SHIPPING lights were never affected — they bake their aim into the
#     OBJ line (right order) and the plugin only ever writes alpha.
#   * This was ONLY ever wrong for lights whose aim comes from a dataref, i.e. the
#     `--debug` copies. That is precisely the "Original right, Debug wrong" verdict from
#     2026-07-28 and the whole 48-permutation axis hunt: a rotation of four slots is not
#     an axis permutation, so that search could never have found it.
# dbgVals stays in the readable order (5..7 = dir, 8 = cone) because the UI, the aim
# maths, the markers and the tests all index it that way; the translation happens once,
# at the wire, in DbgReadArray.
DebugDataRefOrder = (0, 1, 2, 3, 4, 8, 5, 6, 7)
DebugWireNames = ("r", "g", "b", "a", "size", "semi", "dx", "dy", "dz")
DebugPosCount   = 3                       # x y z OFFSET, slots 9..11 of a working value
DebugPosSlot    = DebugParamCount         # where the offset starts in dbgVals
# AIM slots. Pitch/yaw in DEGREES are the authoritative aim and slots 5..7 (dx/dy/dz)
# are DERIVED from them — never the other way round. Two reasons it is stored rather
# than converted on the fly:
#   * a vector -> angles -> vector round trip loses yaw at the poles (straight up has
#     no heading), so editing pitch through 90 would silently reset yaw;
#   * an angle pair cannot express a non-unit vector, which is what broke the
#     interior's cones. Deriving the vector means the tuner can only emit legal aims.
DebugAimSlot    = DebugPosSlot + DebugPosCount    # 12: pitch, 13: yaw
DebugAimCount   = 2
DebugValCount   = DebugParamCount + DebugPosCount + DebugAimCount
# Nudge size, in the value's own units, one button per decade. This replaced a
# Fine/Coarse pair whose step also varied per KIND: two hidden variables meant the
# amount a click moved something was never on screen, and "did that do anything?"
# is the question this window exists to answer. Here the button label IS the amount
# added. One ladder serves every row because each is either a 0..1 fraction (rgb,
# alpha, cone) or metres (pos, size, dir), and 1 -> 0.001 spans both.
# 10 is here for the ANGLE rows: aim is in degrees now, and swinging a light through a
# quadrant at 1 deg a click is 90 clicks. It is a legitimate position step too (10 cm at
# the 0.1 end, 10 m at this one — the latter only useful inside the +-12 m span).
DebugSteps      = (10.0, 1.0, 0.1, 0.01, 0.001)
DebugDefaultStep = 1.0                    # a degree, or a metre — the useful middle
DebugMarkColor = (1.0, 0.0, 1.0)         # magenta — nothing in a cockpit is this color
DebugBlinkHz    = 2.0

# ---- position markers ----------------------------------------------------------------
# A spill light is invisible in mid-air: you only ever see the POOL it casts, and on a
# close-range light the bright spot of that pool is nowhere near the origin. So placing
# one by eye has been guesswork on this project — the standing complaint against both the
# cockpit spots and the screen glow.
#
# The debug OBJ therefore emits, per light, a string of LIGHT_CUSTOM billboards: dot 0 on
# the origin, the rest marching along the aim vector. Billboards draw as sprites in empty
# air, so it reads as a little arrow. Every dot has its own dataref-driven ANIM_trans
# (same rig as DebugPosDataRef) and this plugin recomputes each offset from the light's
# CURRENT direction every frame — no ANIM_rotate in the OBJ, so all the trigonometry is
# here where it can be tested. Keep in step with the DEBUG_MARK_* constants in
# build/build_objs.py.
DebugMarkerDataRef      = "ToLissPhoton/debug/marker"      # per-dot xyz offset array
DebugMarkerShowDataRef  = "ToLissPhoton/debug/markers"     # 0 off, -1 all, n+1 = light n
DebugMarkerParamDataRef = "ToLissPhoton/debug/markparam"   # /0 origin dot, /1 aim tail
# The marker is three parts: the origin, an axis arrow, and a ring of dots on the CONE
# RIM at the arrow's tip. The rim is what was missing from the first version — an axis
# line alone was reported in-sim as "reflects position well but does not seem fully
# oriented with the spill light", which is what a 20 cm line looks like next to a pool
# of light a metre wide. Drawing the actual cone makes aim and spread both judgeable.
DebugMarkerAxisDots = 5
DebugMarkerRimDots  = 4
DebugMarkerDots     = 1 + DebugMarkerAxisDots + DebugMarkerRimDots
DebugMarkerReach    = 0.6         # metres from the light to the arrow tip, nudgeable
DebugMarkerSize     = 0.05        # LIGHT_CUSTOM size, nudgeable in the window
# One colour per part. The whole point is to see WHICH END is the light and where the
# beam edge falls, so these must stay clearly distinct from each other.
DebugMarkerOriginColor = (0.2, 1.0, 0.3, 1.0)   # green  — the light itself
DebugMarkerAxisColor   = (1.0, 0.6, 0.1, 1.0)   # amber  — which way it points
DebugMarkerRimColor    = (0.3, 0.6, 1.0, 1.0)   # blue   — the cone edge
DebugMarkerST      = (0.0, 0.0, 0.5, 0.5)   # light-texture region, as the OBJ bakes it
DebugMarkerRoles   = 3                      # origin / axis / rim -> markparam/0../2
DebugMarkerOff, DebugMarkerAll = 0, -1

# Cardinal aim presets: (label, pitch, yaw). One click puts a light on an UNAMBIGUOUS
# axis, which is the only clean way to answer "which way does the sim think this points".
# Sweeping pitch or yaw and watching the pool move is not clean — an interpolated arc
# through a cockpit full of surfaces can be read several ways, and was (2026-07-30:
# "pitching up causes the light to start pointing to the right ... difficult for me to
# understand"). Click `Up`, see whether the light goes up. That is the whole test.
DebugAimPresets = (("Fwd", 0.0, 0.0), ("Aft", 0.0, 180.0),
                   ("Left", 0.0, -90.0), ("Right", 0.0, 90.0),
                   ("Up", 90.0, 0.0), ("Down", -90.0, 0.0))

# ---- slot probe -----------------------------------------------------------------------
# WHAT WE THINK the nine dataref floats mean, in order. Everything in this tool, in
# plugin.cpp and in the DSL rests on it, and it has never been measured — only read off
# the OBJ8 spec and lights.txt's `airplane_panel_sp` row.
#
# In-sim it does not hold. 2026-07-30, on the screen glow: `aim Fwd` (our dir 0 0 -1)
# points the light STRAIGHT DOWN, editing pitch swings it horizontally, and the cone and
# one of the direction components affect each other. Every one of those is what you get
# if the array is read ONE SLOT LATE — our dy landing on the sim's DX, our dz on its DY,
# our cone on its DZ. Two earlier explanations (an axis permutation, then non-unit `dir`
# lengths) each fitted part of the evidence and were wrong.
#
# So this stops guessing and MEASURES it. Probing slot k sends the baseline below with
# slot k alone set to 1, so whatever the sim does is caused by that slot and nothing
# else. Nine clicks map our array onto the sim's parameters with no inference.
#
# Baseline choices, each load-bearing:
#   * dir 0 0 0 — omnidirectional, so a probe of 5/6/7 introduces the ONLY direction
#     present and there is nothing to disentangle it from.
#   * cone 0 — cos(90 deg). If slot 8 turns out to be a direction component after all,
#     a baseline of 0 contributes nothing to it, keeping probes 5..7 clean.
#   * size 4 m — big enough that the pool lands on real geometry rather than the panel
#     two centimetres away, which is what made cone changes look inert.
#   * white, alpha 1 — no rheostat, no palette, nothing to misread as a colour bug.
DebugProbeBaseline = (1.0, 1.0, 1.0, 1.0, 4.0, 0.0, 0.0, 0.0, 0.0)
DebugProbeValue = 1.0
# What we EXPECT each slot to do, printed beside the probe so the answer is a
# comparison rather than a memory test.
DebugProbeExpect = ("red only", "green only", "blue only", "alpha (already 1)",
                    "bigger", "aim RIGHT (+x)", "aim UP (+y)", "aim AFT (+z)",
                    "NOTHING — baseline dir is 0 0 0, so there is no cone to narrow")
# The probe doubles as the regression test for the wire order: with DebugDataRefOrder
# right, every one of the nine matches its expectation above. It was 5/6/7/8 giving
# omni/right/up/aft — SEMI/DX/DY/DZ, one slot rotated — that measured the order in the
# first place (2026-07-30).


def _debug_marker_role(dot):
    """0 origin, 1 axis, 2 cone rim. ⚠ Must match debug_mark_role in build_objs.py:
    it decides which markparam dataref the OBJ baked into that dot's light line."""
    if dot == 0:
        return 0
    return 1 if dot <= DebugMarkerAxisDots else 2

# ---------------------------------------------------------------- orientation cycler
#
# In-sim 2026-07-28 the debug lights sat in the wrong places and faced the wrong way,
# while the originals were right — yet they were still in DISTINCT places and still
# responded to the position knobs. The baked x/y/z are byte-identical between the two
# lines (asserted by test) and the offsets start at zero, so nothing here moves them.
# The remaining explanation is that the two light forms disagree about which axis is
# which.
#
# So rather than guess, enumerate all 48 signed axis permutations (6 orderings x 8 sign
# combinations) on a button and click until the cockpit snaps into place; the label is
# then the answer. Index 0 is the identity, so a fresh session behaves as before.
def _orientations():
    out = []
    for perm in ((0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)):
        for signs in ((1, 1, 1), (1, 1, -1), (1, -1, 1), (1, -1, -1),
                      (-1, 1, 1), (-1, 1, -1), (-1, -1, 1), (-1, -1, -1)):
            label = " ".join(("-" if signs[i] < 0 else "") + "XYZ"[perm[i]]
                             for i in range(3))
            out.append((perm, signs, label))
    return out


DebugOrientations = _orientations()


def _orient(idx, vec):
    """Apply orientation `idx` to a 3-vector. out[i] = sign[i] * vec[perm[i]]."""
    perm, signs, _ = DebugOrientations[idx % len(DebugOrientations)]
    v = list(vec) + [0.0, 0.0, 0.0]
    return [signs[i] * v[perm[i]] for i in range(3)]
# Brightness source -> the rheostat a light's alpha is multiplied by, keyed by the
# manifest's `source`. LIGHT_SPILL_CUSTOM has no INDEX slot, so without this every debug
# light would ignore the knob it is identified by.
DebugPanelRheostat = "sim/cockpit2/electrical/panel_brightness_ratio"
DebugInstRheostat  = "sim/cockpit2/electrical/instrument_brightness_ratio"
DebugMapRheostat   = "ckpt/lights/map"
# The screen-glow lights ride a per-DISPLAY brightness, not a rheostat: manifest
# `source: "du"`, `index` = the AirbusFBW/DUBrightness slot. Dimming one display
# should move exactly one glow — which is how the kScreenDU seed in plugin.cpp
# gets verified.
DebugDUBrightness  = "AirbusFBW/DUBrightness"

# Command + menu identity
CommandName = "ToLissPhoton/dev/quick_reload"
CommandDesc = "Photon Dev: snapshot -> reload aircraft -> restore state & camera"
PlainReloadCommandName = "ToLissPhoton/dev/plain_reload"
PlainReloadCommandDesc = "Photon Dev: reload the aircraft only (no state or camera restore)"
ReleaseCameraCommandName = "ToLissPhoton/dev/release_camera"
ReleaseCameraCommandDesc = "Photon Dev: release the held camera back to free-look control"
CineToggleCommandName = "ToLissPhoton/dev/cinematic_camera"
CineToggleCommandDesc = "Photon Dev: toggle the slow cinematic camera"
CineFasterCommandName = "ToLissPhoton/dev/cinematic_faster"
CineSlowerCommandName = "ToLissPhoton/dev/cinematic_slower"
# ====================================================================================


# XPLM constants with integer fallbacks (names occasionally differ by version)
CommandBegin    = getattr(xp, "CommandBegin", 0)
CommandContinue = getattr(xp, "CommandContinue", 1)
CommandEnd      = getattr(xp, "CommandEnd", 2)
MsgPlaneLoaded = getattr(xp, "MSG_PLANE_LOADED", 102)
PhaseAfterFM   = getattr(xp, "FlightLoop_Phase_AfterFlightModel", 1)
CamForever     = getattr(xp, "ControlCameraForever", 2)
WinDecoNone    = getattr(xp, "WindowDecorationNone", 0)
WinDecoRound   = getattr(xp, "WindowDecorationRoundRectangle", 1)
WinLayerFloat  = getattr(xp, "WindowLayerFloatingWindows", 2)
WinLayerOverlay = getattr(xp, "WindowLayerFlightOverlay", WinLayerFloat)
WinPositionFree = getattr(xp, "WindowPositionFree", 0)
MouseDown      = getattr(xp, "MouseDown", 1)
MouseUp        = getattr(xp, "MouseUp", 3)
FontProp       = getattr(xp, "Font_Proportional", 18)
CursorDefault  = getattr(xp, "CursorDefault", 0)

TYPE_INT      = getattr(xp, "Type_Int", 1)
TYPE_FLOAT    = getattr(xp, "Type_Float", 2)
TYPE_DOUBLE   = getattr(xp, "Type_Double", 4)
TYPE_FLOATARR = getattr(xp, "Type_FloatArray", 8)
TYPE_INTARR   = getattr(xp, "Type_IntArray", 16)

# state machine. ST_PLAIN is the bare reload: it still disables/re-enables the
# aircraft's own plugins (that is crash avoidance, not convenience) but does no
# dataref snapshot and never touches the camera.
ST_IDLE, ST_AWAIT_LOAD, ST_SETTLING, ST_CAMERA, ST_PLAIN = 0, 1, 2, 3, 4

# camera pose slots, as returned by xp.readCameraPosition()
CAM_X, CAM_Y, CAM_Z, CAM_PITCH, CAM_HEADING, CAM_ROLL, CAM_ZOOM = range(7)


def _log(msg):
    try:
        xp.log("Photon Dev: " + msg)
    except Exception:
        pass


# ============================== tiny immediate-mode UI ================================
# Enough widget to build the two dev windows without a widget library. Buttons are drawn
# by the window's draw callback, which ALSO records each button's rectangle into a hit
# list; the click callback then just searches that list. Layout therefore exists in
# exactly one place and a click can never land somewhere the button isn't - the same
# draw/hit-test-share-geometry discipline the native plugin's Custom window uses, minus
# its duplicated rect maths.
UI_PAD, UI_GAP, UI_TOP_INSET, UI_BTN_PAD = 8, 6, 20, 8
UI_BULLET = "• "

COL_TEXT   = (0.90, 0.90, 0.90)
COL_BRIGHT = (1.0, 1.0, 1.0)
# "Dim" is a de-emphasis, not a low-contrast grey: these windows draw on X-Plane's own
# mid-grey chrome, where 0.55 grey-on-grey was unreadable. Rank by weight, not by
# fading toward the background.
COL_DIM    = (0.80, 0.80, 0.83)
COL_ACTIVE = (0.0, 0.729, 1.0)


def _measure(text):
    try:
        return int(xp.measureString(FontProp, text))
    except Exception:
        return 7 * len(text)


def _font_height():
    try:
        return int(xp.getFontDimensions(FontProp)[1])
    except Exception:
        return 12


def _draw_text(color, x, y, text):
    try:
        xp.drawString(color, x, y, text, None, FontProp)
    except Exception:
        pass


def _step_label(step):
    """A step button's label. %g drops trailing zeros, so the ladder reads
    1 / 0.1 / 0.01 / 0.001 — the exact number a click adds."""
    return "%g" % step


# ---- aim angles ----------------------------------------------------------------------
# Mirrors aim_to_dir / dir_to_aim / spread_to_cone in build/photon_dsl.py, which is where
# the convention is documented and where the DSL's `aim:` and `spread:` are converted.
# The two must agree or a number tuned here means something else when pasted there — a
# test compares them.
#
#   pitch  degrees above horizontal.  0 level, +90 up, -90 down.
#   yaw    aircraft heading: 0 forward (-Z), 90 right (+X), 180 aft (+Z).
# No roll: a cone is rotationally symmetric about its axis, so two angles are all there
# are. A roll knob would be a control that provably does nothing.
def _aim_to_dir(pitch, yaw):
    p, y = math.radians(pitch), math.radians(yaw)
    horiz = math.cos(p)
    return [horiz * math.sin(y), math.sin(p), -horiz * math.cos(y)]


def _dir_to_aim(d):
    length = math.sqrt(sum(v * v for v in d))
    if length < 1e-9:
        return [0.0, 0.0]                      # omnidirectional
    x, yv, z = (v / length for v in d)
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, yv))))
    # At the poles yaw is degenerate; report 0 rather than an arbitrary residue.
    yaw = 0.0 if abs(yv) > 1.0 - 1e-9 else math.degrees(math.atan2(x, -z))
    return [pitch, yaw]


def _cone_to_spread(cone):
    return math.degrees(math.acos(max(-1.0, min(1.0, cone))))


def _spread_to_cone(deg):
    return math.cos(math.radians(max(0.0, min(180.0, deg))))


def _pitch_word(pitch):
    """Plain English for a pitch angle. The sign convention is the one thing about an
    aim you cannot check by eye without already knowing the answer, so the window says
    it rather than making you remember it."""
    if pitch <= -60:
        return "straight down"
    if pitch < -10:
        return "down"
    if pitch <= 10:
        return "level"
    if pitch < 60:
        return "up"
    return "straight up"


def _yaw_word(yaw):
    """Likewise for heading. 0 is FORWARD (-Z), which is the half of the convention
    most easily got backwards."""
    y = (yaw + 180.0) % 360.0 - 180.0
    for limit, word in ((-157.5, "aft"), (-112.5, "aft-left"), (-67.5, "left"),
                        (-22.5, "fwd-left"), (22.5, "forward"), (67.5, "fwd-right"),
                        (112.5, "right"), (157.5, "aft-right")):
        if y < limit:
            return word
    return "aft"


def _perp_basis(d):
    """Two unit vectors perpendicular to `d` and to each other.

    Used to place the cone-rim dots. The seed axis is the world axis `d` leans on
    LEAST, so the cross product can never collapse: its magnitude is at least
    sqrt(2/3). A fixed seed would degenerate for any light aimed along it, and the
    commonest cockpit aims are straight down and straight aft — exactly the axes a
    guess would pick."""
    length = math.sqrt(sum(v * v for v in d)) or 1.0
    n = [v / length for v in d]
    least = min(range(3), key=lambda k: abs(n[k]))
    seed = [0.0, 0.0, 0.0]
    seed[least] = 1.0
    u = [seed[1] * n[2] - seed[2] * n[1],
         seed[2] * n[0] - seed[0] * n[2],
         seed[0] * n[1] - seed[1] * n[0]]
    ulen = math.sqrt(sum(v * v for v in u)) or 1.0
    u = [v / ulen for v in u]
    w = [n[1] * u[2] - n[2] * u[1],
         n[2] * u[0] - n[0] * u[2],
         n[0] * u[1] - n[1] * u[0]]
    return n, u, w


class UiButton:
    """One drawn button plus the action to run when it is clicked."""
    __slots__ = ("l", "b", "r", "t", "action")

    def __init__(self, l, b, r, t, action):
        self.l, self.b, self.r, self.t, self.action = l, b, r, t, action

    def hit(self, x, y):
        return self.l <= x <= self.r and self.b <= y <= self.t


class PythonInterface:
    def __init__(self):
        self.Name = "Photon Dev Reload"
        self.Sig  = "toliss.photon.devreload"
        self.Desc = "Dev tool: one-key reload + state/camera restore for light-position iteration."

        self.state = ST_IDLE
        self.cmdRef = None
        self.plainCmdRef = None
        self.releaseCmdRef = None
        self.cineCmdRef = None
        self.cineFasterCmdRef = None
        self.cineSlowerCmdRef = None
        self.driverLoopID = None

        # menu
        self.pluginsMenuItem = None
        self.menuID = None

        # snapshot buffers
        self.cameraSnap = None          # [x, y, z, pitch, heading, roll, zoom]
        self.datarefSnap = {}           # name -> (kind, value)
        self.disabledPlugins = []       # names we disabled (re-enable after reload)
        self.pluginsReenabled = False

        # settle / quiescence-detection state (see the Driver SETTLING branch)
        self.settleElapsed = 0.0
        self.settleStarted = False      # have we captured the baseline dataref values yet
        self.lastValues = {}            # name -> last observed value (activity detection)
        self.sawActivity = False        # have we seen ToLiss change a snapshot dataref
        self.lastChangeTime = 0.0       # settleElapsed at the last observed change
        self.restored = False           # have we written our snapshot back yet
        self.restoreGuardElapsed = 0.0

        # camera control (persistent hold-until-input, see _begin_camera_hold)
        self.camActive = False          # we WANT to hold the pose (until user input)
        self.camControlling = False     # do we currently OWN the camera (controlCamera)
        self.pendingRegrab = False      # mid re-grab (free-view done, controlCamera next tick)
        self.camHoldDeadline = None     # monotonic safety-cap deadline, or None = no cap
        self.camWatchLoopID = None      # watchdog flight loop that re-grabs on involuntary loss
        self.keySnifferOn = False       # key sniffer registered while holding
        self.mouseWin = None            # full-screen click/wheel catcher while holding

        # cinematic camera (see the CineMoveSpeed tunables)
        self.cineActive = False
        self.cineControlling = False
        self.cinePendingRegrab = False
        self.cinePose = None            # the pose we are driving, mutated every frame
        self.cineLoopID = None
        self.cineHeld = {}              # refCon tuple -> True while its command is held
        self.cineVel = {"fwd": 0.0, "side": 0.0, "up": 0.0,
                        "pitch": 0.0, "hdg": 0.0, "zoom": 0.0}   # smoothed velocities
        self.cineMulIndex = CineDefaultMultiplierIndex
        self.cineLastTick = None
        self.cineHooks = []             # (commandRef, refCon) we registered handlers on
        self.cineWin = None
        self.cineHits = []

        # cockpit light tuner
        self.tunerWin = None
        self.tunerHits = []
        self.tunerPath = None           # the OBJ we parsed
        self.tunerLines = []            # the whole file, split into lines
        self.tunerOrder = []            # keys, in file order
        self.tunerLights = {}           # key -> {"cls", "refs": [(lineIdx, cmd, prefix, nums, tail)]}
        self.tunerVals = {}             # key -> working copy of the 12 numeric slots
        self.tunerSel = 0
        self.tunerCoarse = False
        self.tunerNote = "not loaded"

        # live light debug (the --debug OBJ + its dataref pool)
        self.dbgWin = None
        self.dbgWinSlots = 0            # row count the window's height was sized for
        self.dbgHits = []
        self.dbgAccessors = []          # registered dataref refs, index == light n
        self.dbgPosAccessor = None      # the one shared position-offset array
        self.dbgCompareAccessor = None  # the A/B toggle
        self.dbgMarkerAccessors = []    # marker offset array + show flag + 2 param sets
        # Which position markers are drawn: "off" | "this" | "all". Kept as a MODE and
        # resolved to the dataref's value per read, so "this" tracks the selection —
        # storing sel+1 would leave the arrow behind on the previous light after Next.
        # Off by default: 26 cockpit lights' worth of arrows at once is unreadable, and
        # a debug build should look like the cockpit until you ask it not to.
        # Slot probe: None = normal tuning, 0..8 = send DebugProbeBaseline with that one
        # slot set. See the DebugProbeBaseline comment for why this exists.
        self.dbgProbe = None
        self.dbgMarkerMode = "off"
        self.dbgMarkerSize = DebugMarkerSize
        self.dbgMarkerReach = DebugMarkerReach   # metres to the arrow tip / cone ring
        self.dbgMarkerDots = DebugMarkerDots   # re-read from the manifest
        self.dbgCompare = 1             # 1 = tunable debug copies, 0 = original lines
        self.dbgLights = []             # manifest rows
        # working values, 12 per light: r,g,b,a,size,dx,dy,dz,cone + x,y,z OFFSET
        self.dbgVals = []
        self.dbgBase = []               # the manifest's own values, for Revert
        self.dbgLoadTried = False       # one manifest load attempt per aircraft
        # Does the installed OBJ carry the ANIM_trans position wrappers (manifest's
        # `pos_tunable`)? False hides every position control — a knob that moves nothing
        # reads as the light refusing to move, not as the feature being off.
        self.dbgPosTunable = False
        self.dbgSel = 0
        self.dbgStep = DebugDefaultStep   # metres / units per -/+ click
        self.dbgIsolate = False
        self.dbgBlink = False
        self.dbgMark = False
        # Orientation hypothesis under test, as indices into DebugOrientations; 0 is the
        # identity. Global, NOT per-light — one click re-tests every light at once,
        # because "the whole cockpit snaps into place" is the signal to watch for.
        self.dbgOrientPos = 0
        self.dbgOrientDir = 0
        # Linked by default: a real coordinate-space mismatch would hit position and
        # direction alike, so that 48-click sweep is worth doing first. Unlinking opens
        # the 48x48 case if it finds nothing.
        self.dbgOrientLink = True
        self.dbgNote = "not loaded"
        self.dbgTarget = ""             # which debug OBJ is live: cockpit | screens
        self.dbgRheostatRefs = {}       # name -> dataref handle (or None if missing)
        self.dbgFrameTime = -1.0        # rheostat cache stamp
        self.dbgFrameVals = {}

    # ---------------------------------------------------------------- lifecycle
    def XPluginStart(self):
        try:
            self.cmdRef = xp.createCommand(CommandName, CommandDesc)
            xp.registerCommandHandler(self.cmdRef, self.QuickReloadCmd, 1, 0)
            self.plainCmdRef = xp.createCommand(PlainReloadCommandName, PlainReloadCommandDesc)
            xp.registerCommandHandler(self.plainCmdRef, self.PlainReloadCmd, 1, 0)
            self.releaseCmdRef = xp.createCommand(ReleaseCameraCommandName, ReleaseCameraCommandDesc)
            xp.registerCommandHandler(self.releaseCmdRef, self.ReleaseCameraCmd, 1, 0)
            self.cineCmdRef = xp.createCommand(CineToggleCommandName, CineToggleCommandDesc)
            xp.registerCommandHandler(self.cineCmdRef, self.CineToggleCmd, 1, 0)
            self.cineFasterCmdRef = xp.createCommand(
                CineFasterCommandName, "Photon Dev: cinematic camera - next speed up")
            xp.registerCommandHandler(self.cineFasterCmdRef, self.CineFasterCmd, 1, 0)
            self.cineSlowerCmdRef = xp.createCommand(
                CineSlowerCommandName, "Photon Dev: cinematic camera - next speed down")
            xp.registerCommandHandler(self.cineSlowerCmdRef, self.CineSlowerCmd, 1, 0)
            # MUST be here, not in XPluginEnable: an OBJ binds dataref names when the
            # aircraft loads, and a name registered later stays bound to nothing.
            self._dbg_register_accessors()
            _log("started; commands registered (bind '%s' to a key/button)" % CommandName)
        except Exception:
            _log("XPluginStart error:\n" + traceback.format_exc())
        return self.Name, self.Sig, self.Desc

    def XPluginEnable(self):
        try:
            self.driverLoopID = xp.createFlightLoop(self.Driver, PhaseAfterFM, 0)
            # Separate watchdog loop for the camera hold: it must keep ticking to
            # re-grab the camera after the settle state machine has gone idle.
            self.camWatchLoopID = xp.createFlightLoop(self.CameraWatchLoop, PhaseAfterFM, 0)
            self.cineLoopID = xp.createFlightLoop(self.CineLoop, PhaseAfterFM, 0)
            self.CreateMenu()
        except Exception:
            _log("XPluginEnable error:\n" + traceback.format_exc())
        return 1

    def XPluginDisable(self):
        try:
            self._cine_stop("plugin disable")
            self._release_camera()
            self._destroy_window("cineWin")
            self._destroy_window("tunerWin")
            self._destroy_window("dbgWin")
            for loop in ("driverLoopID", "camWatchLoopID", "cineLoopID"):
                if getattr(self, loop) is not None:
                    xp.destroyFlightLoop(getattr(self, loop))
                    setattr(self, loop, None)
            self.DestroyMenu()
        except Exception:
            _log("XPluginDisable error:\n" + traceback.format_exc())

    def XPluginStop(self):
        try:
            for ref, handler in ((self.cmdRef, self.QuickReloadCmd),
                                 (self.plainCmdRef, self.PlainReloadCmd),
                                 (self.releaseCmdRef, self.ReleaseCameraCmd),
                                 (self.cineCmdRef, self.CineToggleCmd),
                                 (self.cineFasterCmdRef, self.CineFasterCmd),
                                 (self.cineSlowerCmdRef, self.CineSlowerCmd)):
                if ref is not None:
                    xp.unregisterCommandHandler(ref, handler, 1, 0)
            self._dbg_unregister_accessors()
        except Exception:
            _log("XPluginStop error:\n" + traceback.format_exc())

    def XPluginReceiveMessage(self, inFromWho, inMessage, inParam):
        try:
            if inMessage != MsgPlaneLoaded or inParam != 0:
                return
            # A reload re-reads the OBJ, so the manifest beside it may be new, newly
            # present or newly gone. Re-read it and drop the working values with it —
            # edits kept across a reload would describe lights that are no longer there.
            # This is the belt; the lazy load in _dbg_values is the braces.
            self.dbgLoadTried = True
            self._dbg_load()
            # A reload is how a --no-debug-pos build usually replaces a normal one, so the
            # row count may have changed. Message context, so rebuilding is safe here.
            self._dbg_resize_window()
            if self.state == ST_PLAIN:
                # Bare reload: nothing to settle, nothing to restore. The one thing
                # still owed is re-enabling the aircraft's plugins, and that has to
                # wait a beat for them to finish (re)loading - hence a single
                # deferred tick rather than doing it right here.
                _log("plane loaded (plain reload); re-enabling aircraft plugins shortly")
                xp.scheduleFlightLoop(self.driverLoopID, 1.0, 1)
                return
            if self.state == ST_AWAIT_LOAD:
                _log("plane loaded; waiting out ToLiss init before restoring")
                self.state = ST_SETTLING
                self.settleElapsed = 0.0
                self.settleStarted = False
                self.lastValues = {}
                self.sawActivity = False
                self.lastChangeTime = 0.0
                self.restored = False
                self.restoreGuardElapsed = 0.0
                self.pluginsReenabled = False
                xp.scheduleFlightLoop(self.driverLoopID, RestoreTickSeconds, 1)
        except Exception:
            _log("ReceiveMessage error:\n" + traceback.format_exc())

    # ---------------------------------------------------------------- commands
    def QuickReloadCmd(self, inCommand, inPhase, inRefcon):
        if inPhase == CommandBegin:
            self.Trigger()
        return 1

    def PlainReloadCmd(self, inCommand, inPhase, inRefcon):
        if inPhase == CommandBegin:
            self.PlainReload()
        return 1

    def ReleaseCameraCmd(self, inCommand, inPhase, inRefcon):
        if inPhase == CommandBegin:
            self._release_camera_to_free()
        return 1

    def CineToggleCmd(self, inCommand, inPhase, inRefcon):
        if inPhase == CommandBegin:
            self._cine_stop("toggled off") if self.cineActive else self._cine_start()
        return 1

    def CineFasterCmd(self, inCommand, inPhase, inRefcon):
        if inPhase == CommandBegin:
            self._cine_step_multiplier(+1)
        return 1

    def CineSlowerCmd(self, inCommand, inPhase, inRefcon):
        if inPhase == CommandBegin:
            self._cine_step_multiplier(-1)
        return 1

    # ---------------------------------------------------------------- the sequence
    def Trigger(self):
        try:
            if self.state != ST_IDLE:
                _log("busy (state %d) - ignoring trigger" % self.state)
                return
            _log("=== quick reload triggered ===")
            self._snapshot_camera()
            # Release any hold from a PREVIOUS cycle before reloading: the snapshot
            # above already captured the (held) pose, and the hold must not stay
            # active through the coming reload + ToLiss init. Forcing the camera
            # external while ToLiss resets the view mode is what culls the exterior
            # (the settle's whole point is to re-establish the hold only AFTER
            # quiescence, at ST_CAMERA). Quiet when nothing was held.
            self._cine_stop("quick reload")
            self._release_camera()
            self._snapshot_datarefs()
            self._disable_plugins()
            self._issue_reload()
        except Exception:
            _log("Trigger error:\n" + traceback.format_exc())
            self.state = ST_IDLE

    def PlainReload(self):
        """Reload the aircraft and nothing else.

        The quick reload's snapshot/settle/camera-hold machinery exists to make
        light-POSITION iteration painless, and it costs 10-45 seconds of waiting out
        ToLiss's cold-start init. When you only need the OBJ or plugin re-read - and
        are happy to be dropped back in the cockpit with the switches cold - this is
        the same reload without any of that.

        What it deliberately KEEPS is the plugin disable/re-enable around the reload.
        That is not part of the "fix"; it is the crash avoidance that makes reloading
        a ToLiss safe at all (see _disable_plugins), so skipping it would just mean an
        occasional hard sim crash instead of a faster reload.
        """
        try:
            if self.state != ST_IDLE:
                _log("busy (state %d) - ignoring plain reload" % self.state)
                return
            _log("=== plain reload triggered (no state/camera restore) ===")
            # A live camera hold would survive into the reload and fight ToLiss's
            # view reset - the exact thing that culls the exterior. Drop it first.
            self._cine_stop("plain reload")
            self._release_camera()
            self._disable_plugins()
            self._issue_reload(plain=True)
        except Exception:
            _log("PlainReload error:\n" + traceback.format_exc())
            self.state = ST_IDLE

    def _issue_reload(self, plain=False):
        # CRITICAL: issue the reload from THIS command/menu-handler context, never from a
        # flight-loop callback. sim/operation/reload_aircraft runs SYNCHRONOUSLY and unloads
        # the aircraft's plugins; doing that nested inside X-Plane's flight-loop dispatch
        # corrupts the callback teardown ("XPLMFlightLoopRecord ... does not exist", draw
        # callbacks "deleted by a different plugin") and hard-crashes the sim. The manual
        # Developer-menu reload is crash-free precisely because it runs in a handler context
        # like this one - so we match it. (This is why Trigger is only ever called from the
        # command/menu handlers, and the reload is NOT deferred through the flight loop.)
        cmd = xp.findCommand(ReloadCommand)
        if cmd is None:
            _log("MISSING reload command '%s' - aborting; re-enabling plugins" % ReloadCommand)
            self._reenable_plugins()
            self.state = ST_IDLE
            return
        # Set the awaiting state *before* commandOnce: the reload (and MSG_PLANE_LOADED)
        # can fire synchronously inside this call, so ReceiveMessage must already see it.
        self.state = ST_PLAIN if plain else ST_AWAIT_LOAD
        _log("issuing %s (handler context)" % ReloadCommand)
        xp.commandOnce(cmd)
        _log("reload issued; awaiting MSG_PLANE_LOADED")

    def Driver(self, sinceLast, sinceLoop, counter, refCon):
        try:
            if self.state == ST_PLAIN:
                self._reenable_plugins()
                _log("=== plain reload complete ===")
                self.state = ST_IDLE
                return 0

            if self.state == ST_SETTLING:
                self.settleElapsed += sinceLast
                if not self.pluginsReenabled:
                    self._reenable_plugins()
                    self.pluginsReenabled = True

                if not self.restored:
                    # PHASE 1: wait out ToLiss's cold-start init. Capture a baseline on the
                    # first tick, then watch for ToLiss to change the snapshot datarefs and
                    # subsequently go quiet. Restore only once init has quiesced (or a
                    # timeout / hard cap fires) - writing during init would just be clobbered.
                    if not self.settleStarted:
                        self._sample_datarefs()
                        self.settleStarted = True
                        self.lastChangeTime = self.settleElapsed
                        return RestoreTickSeconds
                    if self._datarefs_changed():
                        if not self.sawActivity:
                            _log("ToLiss init activity detected; waiting for it to settle")
                        self.sawActivity = True
                        self.lastChangeTime = self.settleElapsed
                    quietFor = self.settleElapsed - self.lastChangeTime
                    quiesced = self.sawActivity and quietFor >= QuietSeconds
                    timedOut = (not self.sawActivity) and self.settleElapsed >= ActivityTimeoutSeconds
                    capped   = self.settleElapsed >= MaxSettleSeconds
                    if quiesced or timedOut or capped:
                        why = "quiesced" if quiesced else ("no-activity-timeout" if timedOut else "max-cap")
                        _log("ToLiss init done (%s, t=%.1fs); restoring state" % (why, self.settleElapsed))
                        self._restore_datarefs()
                        self._run_restore_commands()
                        self.restored = True
                        self.restoreGuardElapsed = 0.0
                    return RestoreTickSeconds

                # PHASE 2: guard window - re-assert briefly in case ToLiss makes a final
                # tweak right at the end of init, then begin the camera restore.
                self._restore_datarefs()
                self.restoreGuardElapsed += sinceLast
                if self.restoreGuardElapsed >= RestoreGuardSeconds:
                    # Switch to the free external view NOW, but take camera control on the
                    # NEXT tick (ST_CAMERA). Doing both in one frame trips ControlCamera
                    # UntilViewChanges - the view switch itself counts as a view change and
                    # releases us instantly, which is why the camera restore did nothing.
                    self._enter_free_camera()
                    self.state = ST_CAMERA
                return RestoreTickSeconds

            if self.state == ST_CAMERA:
                # A frame after the view switch: take camera control and HOLD the saved pose,
                # re-asserting it every frame until the user gives keyboard/mouse input (or the
                # safety cap fires). The hold runs off ControlCameraForever + input watchers,
                # independent of this state machine, so we can go idle immediately.
                self._begin_camera_hold()
                _log("=== settle complete; state restored (holding camera until input) ===")
                self.state = ST_IDLE
                return 0
        except Exception:
            _log("Driver error:\n" + traceback.format_exc())
            self.state = ST_IDLE
            return 0
        return 0

    # ---------------------------------------------------------------- snapshot / restore
    def _snapshot_camera(self):
        try:
            pos = xp.readCameraPosition()
            self.cameraSnap = [float(pos[i]) for i in range(7)]
            viewType = "?"
            try:
                ref = xp.findDataRef(ViewTypeDataRef)
                if ref is not None:
                    viewType = str(xp.getDatai(ref))
            except Exception:
                pass
            _log("camera snapshot: x=%.1f y=%.1f z=%.1f pitch=%.1f hdg=%.1f roll=%.1f zoom=%.2f view_type=%s"
                 % (tuple(self.cameraSnap) + (viewType,)))
        except Exception:
            self.cameraSnap = None
            _log("camera snapshot failed:\n" + traceback.format_exc())

    def _snapshot_datarefs(self):
        self.datarefSnap = {}
        for name in SnapshotDataRefs:
            snap = self._read_ref(name)
            if snap is None:
                _log("dataref MISSING (name wrong or not present): %s" % name)
            else:
                self.datarefSnap[name] = snap
                _log("dataref snapshot: %s = %r (%s)" % (name, snap[1], snap[0]))

    def _restore_datarefs(self):
        for name, snap in self.datarefSnap.items():
            self._write_ref(name, snap)

    def _sample_datarefs(self):
        """Seed lastValues with the current reads so the first real ToLiss write reads as a
        change (activity), not a false positive against an empty baseline."""
        for name in self.datarefSnap:
            cur = self._read_ref(name)
            if cur is not None:
                self.lastValues[name] = cur[1]

    def _datarefs_changed(self):
        """True if any snapshot dataref changed since the last poll; updates lastValues.
        (List values compare element-wise, so array datarefs like OHPLightSwitches work.)"""
        changed = False
        for name in self.datarefSnap:
            cur = self._read_ref(name)
            if cur is None:
                continue
            if self.lastValues.get(name) != cur[1]:
                changed = True
                self.lastValues[name] = cur[1]
        return changed

    def _run_restore_commands(self):
        for name in RestoreCommands:
            try:
                cmd = xp.findCommand(name)
                if cmd is not None:
                    xp.commandOnce(cmd)
                    _log("ran restore command: %s" % name)
                else:
                    _log("restore command MISSING: %s" % name)
            except Exception:
                _log("restore command error %s:\n%s" % (name, traceback.format_exc()))

    def _parse_ref(self, name):
        """Split "path[idx]" into (baseName, index); index is None if not subscripted."""
        if name.endswith("]") and "[" in name:
            base, rest = name[:-1].split("[", 1)
            try:
                return base, int(rest)
            except ValueError:
                return name, None
        return name, None

    def _read_ref(self, name):
        """Return (kind, value) for a dataref, auto-detecting its type, or None if absent.
        Supports a single-element subscript, e.g. "AirbusFBW/ElecOHPArray[3]", which
        snapshots/restores ONLY that element and leaves the rest of the array to ToLiss."""
        try:
            base, idx = self._parse_ref(name)
            ref = xp.findDataRef(base)
            if ref is None:
                return None
            types = 0
            try:
                types = xp.getDataRefTypes(ref)
            except Exception:
                types = 0
            if idx is not None:
                # single array element: value stored as (index, value)
                if (types & TYPE_FLOATARR) and not (types & TYPE_INTARR):
                    vals = []
                    xp.getDatavf(ref, vals, idx, 1)
                    return ("floatelem", (idx, vals[0] if vals else 0.0))
                vals = []
                xp.getDatavi(ref, vals, idx, 1)
                return ("intelem", (idx, vals[0] if vals else 0))
            if types & TYPE_INTARR:
                vals = []
                n = xp.getDatavi(ref, vals, 0, MaxArrayElements)
                return ("intarray", list(vals[:n]) if n else list(vals))
            if types & TYPE_FLOATARR:
                vals = []
                n = xp.getDatavf(ref, vals, 0, MaxArrayElements)
                return ("floatarray", list(vals[:n]) if n else list(vals))
            if types & TYPE_INT:
                return ("int", xp.getDatai(ref))
            if types & TYPE_FLOAT:
                return ("float", xp.getDataf(ref))
            if types & TYPE_DOUBLE:
                return ("double", xp.getDatad(ref))
            # unknown/unadvertised type: best-effort int read
            return ("int", xp.getDatai(ref))
        except Exception:
            _log("read %s failed:\n%s" % (name, traceback.format_exc()))
            return None

    def _write_ref(self, name, snap):
        kind, val = snap
        try:
            base, idx = self._parse_ref(name)
            ref = xp.findDataRef(base)
            if ref is None:
                return
            if kind == "intelem":
                i, v = val
                xp.setDatavi(ref, [v], i, 1)
            elif kind == "floatelem":
                i, v = val
                xp.setDatavf(ref, [v], i, 1)
            elif kind == "intarray":
                xp.setDatavi(ref, val, 0, len(val))
            elif kind == "floatarray":
                xp.setDatavf(ref, val, 0, len(val))
            elif kind == "int":
                xp.setDatai(ref, val)
            elif kind == "float":
                xp.setDataf(ref, val)
            elif kind == "double":
                xp.setDatad(ref, val)
        except Exception:
            _log("write %s failed:\n%s" % (name, traceback.format_exc()))

    # ---------------------------------------------------------------- plugin enable/disable
    def _iter_plugins(self):
        """Yield (pluginID, name, signature, filePath) for every plugin except ourselves."""
        try:
            myID = xp.getMyID()
            count = xp.countPlugins()
        except Exception:
            _log("plugin enumeration unavailable:\n" + traceback.format_exc())
            return
        for i in range(count):
            try:
                pid = xp.getNthPlugin(i)
                if pid == myID:
                    continue
                info = xp.getPluginInfo(pid)
                name = getattr(info, "name", None)
                sig = getattr(info, "signature", None)
                path = getattr(info, "filePath", None)
                if name is None and isinstance(info, (list, tuple)):
                    name = info[0] if len(info) > 0 else ""
                    path = info[1] if len(info) > 1 else ""
                    sig = info[2] if len(info) > 2 else ""
                yield pid, (name or ""), (sig or ""), (path or "")
            except Exception:
                continue

    def _current_aircraft_dir(self):
        """Normalised folder of the user's aircraft (.acf), or None if undeterminable."""
        try:
            fileName, path = xp.getNthAircraftModel(0)
            if path:
                return os.path.normcase(os.path.dirname(os.path.abspath(path)))
        except Exception:
            _log("could not resolve aircraft folder:\n" + traceback.format_exc())
        return None

    def _is_aircraft_plugin(self, pluginPath, aircraftDir):
        """True iff the plugin binary lives inside the current aircraft's folder tree.
        This is the safe scope: it can never match a scenery/global plugin (e.g. a SASL
        airport add-on), only ToLiss's own AirbusFBW / SASL plugins."""
        if not pluginPath or not aircraftDir:
            return False
        low = os.path.normcase(os.path.abspath(pluginPath))
        return low == aircraftDir or low.startswith(aircraftDir + os.sep)

    def _skip(self, name):
        # Never-disable exclusions (SkipPluginNameMatches) - e.g. librain/KOSP, which crash
        # if disabled/reloaded mid-session even though they live in the aircraft folder.
        low = name.lower()
        return any(m in low for m in SkipPluginNameMatches)

    def _name_ok(self, name):
        # Optional extra restriction (DisablePluginNameMatches). Empty tuple = accept all
        # aircraft-folder plugins, matching the manual "disable the aircraft's plugins" step.
        if not DisablePluginNameMatches:
            return True
        low = name.lower()
        return any(m in low for m in DisablePluginNameMatches)

    def _should_disable(self, name, path, aircraftDir):
        return (self._is_aircraft_plugin(path, aircraftDir)
                and self._name_ok(name) and not self._skip(name))

    def _disable_plugins(self):
        self.disabledPlugins = []
        aircraftDir = self._current_aircraft_dir()
        if aircraftDir is None:
            _log("aircraft folder unknown - disabling NOTHING (safe). Reload may crash "
                 "if ToLiss's plugins are active; retry once the aircraft is fully loaded.")
            return
        for pid, name, sig, path in self._iter_plugins():
            if not self._should_disable(name, path, aircraftDir):
                continue
            try:
                if xp.isPluginEnabled(pid):
                    xp.disablePlugin(pid)
                    self.disabledPlugins.append(name)
                    _log("disabled aircraft plugin: %s" % name)
            except Exception:
                _log("could not disable %s:\n%s" % (name, traceback.format_exc()))
        if not self.disabledPlugins:
            _log("no aircraft-folder plugins found to disable under %s" % aircraftDir)

    def _reenable_plugins(self):
        # After the reload the aircraft's plugins are freshly (re)loaded and normally already
        # enabled - re-find them (by aircraft-folder scope) and enable any that aren't, so we
        # never leave ToLiss's own plugins disabled.
        aircraftDir = self._current_aircraft_dir()
        if aircraftDir is None:
            return
        for pid, name, sig, path in self._iter_plugins():
            if not self._should_disable(name, path, aircraftDir):
                continue
            try:
                if not xp.isPluginEnabled(pid):
                    xp.enablePlugin(pid)
                    _log("re-enabled aircraft plugin: %s" % name)
            except Exception:
                _log("could not enable %s:\n%s" % (name, traceback.format_exc()))

    # ---------------------------------------------------------------- camera restore
    def _enter_free_camera(self):
        # Step 1: switch into the free external view, so that when we later release control
        # the sim keeps an external camera (not the 3D cockpit the reload snaps back to).
        # Control is taken a frame LATER (see ST_CAMERA) to avoid the view-change race.
        try:
            cmd = xp.findCommand(FreeCameraCommand)
            if cmd is not None:
                xp.commandOnce(cmd)
                _log("switched to free camera view")
            else:
                _log("free-camera command '%s' not found; will still position the camera"
                     % FreeCameraCommand)
        except Exception:
            _log("enter free camera failed:\n" + traceback.format_exc())

    def _begin_camera_hold(self):
        # Step 2 (next frame, view already switched to free camera): take camera control and
        # HOLD the saved pose, re-writing it every CameraFunc callback. (X-Plane exposes no
        # writable dataref for the external camera position - view_x/y/z are read-only in
        # DataRefs.txt - so controlCamera is the only way to place it.) The hold persists
        # INDEFINITELY: CameraWatchLoop re-grabs the camera whenever ToLiss/the sim takes
        # ownership from us (inIsLosingControl), so a late view steal no longer ends it. Only
        # the user's own keyboard/mouse input (the watchers below) or the manual Release Camera
        # command ends it - plus MaxCameraHoldSeconds, if a positive cap was configured. This is
        # only safe because the settle above waits for true quiescence first; holding while
        # ToLiss was still resetting the view mode is what culled the exterior.
        if self.cameraSnap is None:
            return
        try:
            self.camActive = True
            self.pendingRegrab = False
            self.camHoldDeadline = (time.monotonic() + MaxCameraHoldSeconds
                                    if MaxCameraHoldSeconds > 0 else None)
            self._start_input_watchers()
            self._grab_camera()
            if self.camWatchLoopID is not None:
                xp.scheduleFlightLoop(self.camWatchLoopID, CameraRegrabInterval, 1)
            cap = ("cap %.0fs" % MaxCameraHoldSeconds) if self.camHoldDeadline else "no time cap"
            _log("holding camera pose until input (%s); re-grabs on ToLiss steal" % cap)
        except Exception:
            _log("begin camera hold failed:\n" + traceback.format_exc())

    def _grab_camera(self):
        """(Re-)take camera ownership and start driving the saved pose."""
        xp.controlCamera(CamForever, self.CameraFunc, 0)
        self.camControlling = True

    def CameraFunc(self, outCameraPosition, inIsLosingControl, inRefcon):
        try:
            if not self.camActive or self.cameraSnap is None:
                # We intend to stop (user released / teardown). Returning 0 relinquishes;
                # no re-entrant dontControlCamera from inside the callback.
                self.camControlling = False
                return 0
            if inIsLosingControl:
                # Something took the camera from us while we still WANT it (ToLiss's post-load
                # view reset, or the sim). Do NOT tear down: drop the ownership flag and let
                # CameraWatchLoop re-grab. This is the whole fix - losing control used to end
                # the hold, which is why ToLiss "ultimately" won.
                self.camControlling = False
                return 0
            if self.camHoldDeadline is not None and time.monotonic() >= self.camHoldDeadline:
                self._end_camera_hold("safety-cap", call_dont=False)
                return 0
            for i in range(7):
                outCameraPosition[i] = self.cameraSnap[i]
            return 1
        except Exception:
            _log("CameraFunc error:\n" + traceback.format_exc())
            self._end_camera_hold("error", call_dont=False)
            return 0

    def CameraWatchLoop(self, sinceLast, sinceLoop, counter, refCon):
        # Watchdog that makes the hold survive ToLiss's late camera steals. While we want the
        # hold (camActive) but no longer own the camera (camControlling went False in
        # CameraFunc's losing-control branch), re-assert: switch back to the free external view,
        # then re-take controlCamera a frame LATER. The frame gap matters - a view switch and
        # controlCamera in the same frame can release instantly (the documented same-frame
        # view-change race), which is exactly why the initial grab was also split across frames.
        try:
            if not self.camActive:
                return 0  # hold ended; teardown handled dontControlCamera + watchers
            if self.camHoldDeadline is not None and time.monotonic() >= self.camHoldDeadline:
                # Enforce the optional safety cap here too, not only in CameraFunc:
                # during a steal/re-grab window CameraFunc isn't running, so the
                # watchdog is what guarantees the cap still fires.
                self._end_camera_hold("safety-cap")
                return 0
            if self.camControlling:
                return CameraRegrabInterval  # still holding cleanly; just keep polling
            if not self.pendingRegrab:
                self._enter_free_camera()   # get back to the external free view first
                self.pendingRegrab = True
                return -1                   # ...and re-grab on the very next frame
            self._grab_camera()
            self.pendingRegrab = False
            _log("camera re-grabbed after external steal (holding pose)")
            return CameraRegrabInterval
        except Exception:
            _log("CameraWatchLoop error:\n" + traceback.format_exc())
            return 0

    # ------- input watchers: release the hold on the user's keyboard/mouse input ----------
    # A key sniffer catches any key; a full-screen, click-through window catches mouse clicks,
    # right-clicks and the scroll wheel. All handlers PASS THE INPUT THROUGH (return 1 / 0 =
    # "not handled") so the very click/key that ends the hold also does its normal thing - e.g.
    # a right-click-drag both releases us AND begins the sim's free-camera look. Passive cursor
    # motion is deliberately NOT a trigger (these fire only on real clicks/keys/wheel).
    def _start_input_watchers(self):
        try:
            if not self.keySnifferOn and hasattr(xp, "registerKeySniffer"):
                if xp.registerKeySniffer(self.KeySniffer, 1, 0):
                    self.keySnifferOn = True
        except Exception:
            _log("key sniffer register failed:\n" + traceback.format_exc())
        try:
            if self.mouseWin is None and hasattr(xp, "createWindowEx"):
                l, t, r, b = xp.getScreenBoundsGlobal()
                self.mouseWin = xp.createWindowEx(
                    left=l, top=t, right=r, bottom=b, visible=1,
                    draw=self._win_draw, click=self._win_click,
                    wheel=self._win_wheel, rightClick=self._win_rclick,
                    refCon=0, decoration=WinDecoNone, layer=WinLayerOverlay)
        except Exception:
            _log("mouse catcher window failed:\n" + traceback.format_exc())

    def _stop_input_watchers(self):
        if self.keySnifferOn:
            try:
                xp.unregisterKeySniffer(self.KeySniffer, 1, 0)
            except Exception:
                pass
            self.keySnifferOn = False
        if self.mouseWin is not None:
            try:
                xp.destroyWindow(self.mouseWin)
            except Exception:
                pass
            self.mouseWin = None

    def KeySniffer(self, inChar, inFlags, inVirtualKey, inRefcon):
        if self.camActive:
            self._end_camera_hold("keyboard")
        return 1  # never consume the key - let X-Plane handle it normally

    def _win_draw(self, windowID, refCon):
        return

    def _win_click(self, windowID, x, y, inMouse, refCon):
        if self.camActive:
            self._end_camera_hold("mouse-click")
        return 0  # pass through

    def _win_rclick(self, windowID, x, y, inMouse, refCon):
        if self.camActive:
            self._end_camera_hold("mouse-rightclick")
        return 0  # pass through (so the right-drag starts the sim's free-camera look)

    def _win_wheel(self, windowID, x, y, wheel, clicks, refCon):
        if self.camActive:
            self._end_camera_hold("mouse-wheel")
        return 0  # pass through

    def _end_camera_hold(self, reason, call_dont=True):
        was = self.camActive or self.keySnifferOn or self.mouseWin is not None
        self.camActive = False          # stops CameraWatchLoop from re-grabbing
        self.camControlling = False
        self.pendingRegrab = False
        self._stop_input_watchers()
        if self.camWatchLoopID is not None:
            try:
                xp.scheduleFlightLoop(self.camWatchLoopID, 0, 1)  # 0 = deactivate the watchdog
            except Exception:
                pass
        if call_dont:
            try:
                xp.dontControlCamera()
            except Exception:
                pass
        if was:
            _log("camera hold released (%s)" % reason)

    def _release_camera(self):
        # Unconditional teardown (plugin disable / stop).
        self._end_camera_hold("released")

    def _release_camera_to_free(self):
        # Manual "give me control now": re-assert the free-camera view then relinquish.
        try:
            cmd = xp.findCommand(FreeCameraCommand)
            if cmd is not None:
                xp.commandOnce(cmd)
        except Exception:
            pass
        self._release_camera()

    # ============================== shared window plumbing ==========================
    def _create_window(self, title, width, height, drawFn, clickFn):
        try:
            sl, st, sr, sb = xp.getScreenBoundsGlobal()
            left = (sl + sr) // 2 - width // 2
            top = (st + sb) // 2 + height // 2
            win = xp.createWindowEx(
                left=left, top=top, right=left + width, bottom=top - height, visible=1,
                draw=drawFn, click=clickFn,
                key=self._win_noop_key, cursor=self._win_noop_cursor,
                wheel=self._win_noop_wheel, rightClick=self._win_noop_click,
                refCon=0, decoration=WinDecoRound, layer=WinLayerFloat)
            xp.setWindowTitle(win, title)
            xp.setWindowPositioningMode(win, WinPositionFree, -1)
            xp.setWindowResizingLimits(win, width, height, width, height)
            return win
        except Exception:
            _log("could not create window '%s':\n%s" % (title, traceback.format_exc()))
            return None

    def _destroy_window(self, attr):
        win = getattr(self, attr, None)
        if win is not None:
            try:
                xp.destroyWindow(win)
            except Exception:
                pass
            setattr(self, attr, None)

    def _win_noop_key(self, *a):
        return

    def _win_noop_cursor(self, *a):
        return CursorDefault

    def _win_noop_wheel(self, *a):
        return 0

    def _win_noop_click(self, *a):
        return 1

    def _ui_button(self, l, b, r, t, mx, my, hits, label, action, active=False):
        """Draw one button and record its rectangle + action in `hits`."""
        try:
            xp.drawTranslucentDarkBox(l, t, r, b)
        except Exception:
            pass
        over = l <= mx <= r and b <= my <= t
        color = COL_ACTIVE if active else (COL_BRIGHT if over else COL_TEXT)
        text = (UI_BULLET + label) if active else label
        _draw_text(color, l + ((r - l) - _measure(text)) // 2,
                   b + (t - b - _font_height()) // 2 + 1, text)
        hits.append(UiButton(l, b, r, t, action))

    def _ui_button_row(self, x, b, t, mx, my, hits, items, minWidth=0):
        """Draw `items` = [(label, action[, active])] left to right from x."""
        for item in items:
            label, action = item[0], item[1]
            active = item[2] if len(item) > 2 else False
            w = max(minWidth, _measure(label) + 2 * UI_BTN_PAD + (_measure(UI_BULLET) if active else 0))
            self._ui_button(x, b, x + w, t, mx, my, hits, label, action, active)
            x += w + UI_GAP
        return x

    def _ui_click(self, hits, x, y, inMouse):
        # Act on mouse-UP so a press that drags off the button does nothing, and so a
        # single click can never fire twice. The hit list was built by the last draw,
        # which by definition ran before this click.
        if inMouse == MouseUp:
            for btn in hits:
                if btn.hit(x, y):
                    try:
                        btn.action()
                    except Exception:
                        _log("UI action error:\n" + traceback.format_exc())
                    break
        return 1

    # ============================ cinematic camera ==================================
    # Slow, smoothed camera for recording pans. See the CineMoveSpeed tunables for why
    # this drives the camera itself instead of scaling the sim's own motion.
    #
    # AXES are camera-relative, which is what makes a pan feel natural: "forward" goes
    # where you are LOOKING (pitch included), "side" strafes level, "up" is world up
    # (deliberately NOT the camera's up - a level rise while pitched down is what you
    # want for a crane shot, not a diagonal drift).

    # sim command -> (axis, sign). Every one of these gets a _fast and _slow sibling.
    CINE_COMMANDS = (
        ("sim/general/forward",   "fwd",   +1.0),
        ("sim/general/backward",  "fwd",   -1.0),
        ("sim/general/right",     "side",  +1.0),
        ("sim/general/left",      "side",  -1.0),
        ("sim/general/up",        "up",    +1.0),
        ("sim/general/down",      "up",    -1.0),
        ("sim/general/rot_up",    "pitch", +1.0),
        ("sim/general/rot_down",  "pitch", -1.0),
        ("sim/general/rot_right", "hdg",   +1.0),
        ("sim/general/rot_left",  "hdg",   -1.0),
        ("sim/general/zoom_in",   "zoom",  +1.0),
        ("sim/general/zoom_out",  "zoom",  -1.0),
    )

    def _cine_multiplier(self):
        return CineMultipliers[self.cineMulIndex]

    def _cine_step_multiplier(self, delta):
        i = self.cineMulIndex + delta
        self.cineMulIndex = max(0, min(len(CineMultipliers) - 1, i))
        _log("cinematic camera speed x%.2f (%.3f m/s, %.2f deg/s)"
             % (self._cine_multiplier(),
                CineMoveSpeed * self._cine_multiplier(),
                CineRotateSpeed * self._cine_multiplier()))

    def _cine_hook_commands(self):
        """Take over the sim's view commands so existing bindings fly OUR camera.

        Registered in the BEFORE phase and returning 0 while cinematic mode is on, so
        X-Plane never sees them and cannot also move its own camera. When the mode is
        off the handlers return 1 and every binding behaves exactly as it always did -
        which is why it is safe to leave them registered for the session.
        """
        if self.cineHooks:
            return
        for name, axis, sign in self.CINE_COMMANDS:
            for suffix, scale in (("", 1.0), ("_fast", CineFastScale), ("_slow", CineSlowScale)):
                try:
                    ref = xp.findCommand(name + suffix)
                    if ref is None:
                        continue
                    refCon = (axis, sign, scale)
                    xp.registerCommandHandler(ref, self.CineAxisCmd, 1, refCon)
                    self.cineHooks.append((ref, refCon))
                except Exception:
                    _log("could not hook %s%s:\n%s" % (name, suffix, traceback.format_exc()))
        _log("cinematic camera: hooked %d view commands" % len(self.cineHooks))

    def _cine_unhook_commands(self):
        for ref, refCon in self.cineHooks:
            try:
                xp.unregisterCommandHandler(ref, self.CineAxisCmd, 1, refCon)
            except Exception:
                pass
        self.cineHooks = []

    def CineAxisCmd(self, inCommand, inPhase, inRefcon):
        # inRefcon is the (axis, sign, scale) tuple this command was registered with.
        if not self.cineActive:
            return 1                      # mode off: hand the command straight to the sim
        try:
            if inPhase == CommandEnd:
                self.cineHeld.pop(inRefcon, None)
            else:
                self.cineHeld[inRefcon] = True
        except Exception:
            _log("CineAxisCmd error:\n" + traceback.format_exc())
        return 0                          # consumed - the sim must not also move the view

    def _cine_start(self):
        try:
            if self.cineActive:
                return
            # The quick-reload hold and this both call controlCamera; they cannot both
            # own the camera, so the hold yields.
            self._end_camera_hold("cinematic camera")
            pos = xp.readCameraPosition()
            self.cinePose = [float(pos[i]) for i in range(7)]
            self.cineActive = True
            self.cineControlling = False
            # _enter_free_camera runs below, so the first CineLoop tick may grab straight
            # away rather than switching views again.
            self.cinePendingRegrab = True
            self.cineHeld = {}
            for k in self.cineVel:
                self.cineVel[k] = 0.0
            self.cineLastTick = time.monotonic()
            self._cine_hook_commands()
            self._enter_free_camera()
            # Take control on the NEXT frame, not this one: a view switch in the same
            # frame as controlCamera releases us instantly (same race the reload hold
            # splits across frames).
            if self.cineLoopID is not None:
                xp.scheduleFlightLoop(self.cineLoopID, -1, 1)
            _log("cinematic camera ON (x%.2f) - your view keys now fly it"
                 % self._cine_multiplier())
        except Exception:
            _log("cine start error:\n" + traceback.format_exc())
            self.cineActive = False

    def _cine_stop(self, reason):
        if not (self.cineActive or self.cineHooks):
            return
        self.cineActive = False
        self.cineControlling = False
        self.cinePendingRegrab = False
        self.cineHeld = {}
        self._cine_unhook_commands()
        if self.cineLoopID is not None:
            try:
                xp.scheduleFlightLoop(self.cineLoopID, 0, 1)
            except Exception:
                pass
        try:
            xp.dontControlCamera()
        except Exception:
            pass
        _log("cinematic camera OFF (%s)" % reason)

    def _cine_target_velocity(self):
        """Sum the held commands into a per-axis target, in units/second."""
        mul = self._cine_multiplier()
        target = {"fwd": 0.0, "side": 0.0, "up": 0.0, "pitch": 0.0, "hdg": 0.0, "zoom": 0.0}
        for axis, sign, scale in self.cineHeld:
            base = (CineRotateSpeed if axis in ("pitch", "hdg")
                    else CineZoomSpeed if axis == "zoom" else CineMoveSpeed)
            target[axis] += sign * scale * base * mul
        return target

    def _cine_integrate(self, dt):
        """Ease the velocities toward their targets, then advance the pose by dt.

        The easing is a first-order lag with time constant CineSmoothingSeconds -
        `alpha = 1 - exp(-dt/tau)` rather than a fixed per-frame fraction, so the ramp
        takes the same wall-clock time at 30 fps as at 120 and a recording doesn't
        change character with framerate.
        """
        target = self._cine_target_velocity()
        tau = CineSmoothingSeconds
        alpha = 1.0 if tau <= 0.0 else 1.0 - math.exp(-dt / tau)
        for axis in self.cineVel:
            self.cineVel[axis] += (target[axis] - self.cineVel[axis]) * alpha

        pose = self.cinePose
        pose[CAM_PITCH] = max(-89.9, min(89.9, pose[CAM_PITCH] + self.cineVel["pitch"] * dt))
        pose[CAM_HEADING] = (pose[CAM_HEADING] + self.cineVel["hdg"] * dt) % 360.0

        # X-Plane local coords: +x east, +y up, +z SOUTH; heading 0 = north = -z.
        psi = math.radians(pose[CAM_HEADING])
        theta = math.radians(pose[CAM_PITCH])
        cosT = math.cos(theta)
        fwd = (math.sin(psi) * cosT, math.sin(theta), -math.cos(psi) * cosT)
        side = (math.cos(psi), 0.0, math.sin(psi))
        for i, axis in enumerate((CAM_X, CAM_Y, CAM_Z)):
            pose[axis] += (fwd[i] * self.cineVel["fwd"] + side[i] * self.cineVel["side"]) * dt
        pose[CAM_Y] += self.cineVel["up"] * dt

        if self.cineVel["zoom"]:
            pose[CAM_ZOOM] = max(0.05, pose[CAM_ZOOM] + self.cineVel["zoom"] * dt)

    def CineCameraFunc(self, outCameraPosition, inIsLosingControl, inRefcon):
        try:
            if not self.cineActive or self.cinePose is None:
                self.cineControlling = False
                return 0
            if inIsLosingControl:
                # Same treatment as the reload hold: drop the ownership flag and let
                # CineLoop re-grab, rather than ending the mode.
                self.cineControlling = False
                return 0
            now = time.monotonic()
            dt = 0.0 if self.cineLastTick is None else now - self.cineLastTick
            self.cineLastTick = now
            if dt > 0.25:
                dt = 0.25          # a stall must not fling the camera across the field
            if dt > 0.0:
                self._cine_integrate(dt)
            for i in range(7):
                outCameraPosition[i] = self.cinePose[i]
            return 1
        except Exception:
            _log("CineCameraFunc error:\n" + traceback.format_exc())
            self._cine_stop("error")
            return 0

    def CineLoop(self, sinceLast, sinceLoop, counter, refCon):
        # Grabs the camera one frame after the view switch, then acts as the watchdog
        # that re-grabs if the sim or ToLiss takes ownership back.
        try:
            if not self.cineActive:
                return 0
            if self.cineControlling:
                return CameraRegrabInterval       # holding cleanly; just keep polling
            if not self.cinePendingRegrab:
                # Lost ownership. Re-enter the free external view FIRST and grab on the
                # next frame - a view switch and controlCamera in the same frame release
                # instantly (the same race _begin_camera_hold splits across frames).
                self._enter_free_camera()
                self.cinePendingRegrab = True
                return -1
            xp.controlCamera(CamForever, self.CineCameraFunc, 0)
            self.cineControlling = True
            self.cinePendingRegrab = False
            self.cineLastTick = time.monotonic()   # don't integrate the grab gap
            return CameraRegrabInterval
        except Exception:
            _log("CineLoop error:\n" + traceback.format_exc())
            return 0

    # ---------------------------------------------------------------- cinematic window
    def _cine_window_rows(self):
        mul = self._cine_multiplier()
        return [
            ("Cinematic camera", "ON" if self.cineActive else "off"),
            ("Speed multiplier", "x%g" % mul),
            ("Move", "%.3f m/s" % (CineMoveSpeed * mul)),
            ("Rotate", "%.2f deg/s" % (CineRotateSpeed * mul)),
            ("Ease in/out", "%.2f s" % CineSmoothingSeconds),
        ]

    def CineDraw(self, windowID, refCon):
        try:
            l, t, r, b = xp.getWindowGeometry(windowID)
            mx, my = xp.getMouseLocationGlobal()
            fh = _font_height()
            lineH, btnH = fh + 8, fh + 10
            self.cineHits = []

            y = t - UI_TOP_INSET
            for label, value in self._cine_window_rows():
                _draw_text(COL_DIM, l + UI_PAD, y, label)
                _draw_text(COL_ACTIVE if label == "Cinematic camera" and self.cineActive
                           else COL_BRIGHT,
                           l + UI_PAD + 130, y, value)
                y -= lineH

            y -= UI_GAP
            self._ui_button_row(l + UI_PAD, y - btnH, y, mx, my, self.cineHits, [
                ("Slower", lambda: self._cine_step_multiplier(-1)),
                ("Faster", lambda: self._cine_step_multiplier(+1)),
                ("Stop" if self.cineActive else "Start",
                 lambda: self._cine_stop("window") if self.cineActive else self._cine_start()),
            ])
            y -= btnH + UI_GAP + 4
            _draw_text(COL_DIM, l + UI_PAD, y,
                       "Your view keys/hat fly the camera while ON.")
        except Exception:
            _log("CineDraw error:\n" + traceback.format_exc())

    def ShowCineWindow(self):
        if self.cineWin is not None:
            xp.setWindowIsVisible(self.cineWin, 1)
            xp.bringWindowToFront(self.cineWin)
            return
        # Mirrors CineDraw's vertical walk exactly, so nothing lands outside the frame.
        fh = _font_height()
        lineH, btnH = fh + 8, fh + 10
        height = (UI_TOP_INSET + len(self._cine_window_rows()) * lineH
                  + UI_GAP + btnH + UI_GAP + 4 + fh + UI_PAD * 2)
        self.cineWin = self._create_window("Photon Dev - Cinematic Camera", 340, height,
                                           self.CineDraw, self.CineClick)

    def CineClick(self, windowID, x, y, inMouse, refCon):
        return self._ui_click(self.cineHits, x, y, inMouse)

    # ============================ cockpit light tuner ===============================
    # Find a light's position/aim/cone/size in-sim, then paste the answer into the DSL.
    #
    # An OBJ light's geometry is read ONCE, at aircraft load - there is no dataref that
    # moves it afterwards - so every change here means rewriting lights_inn.obj and
    # reloading. That is why edits are staged in memory and applied in one batch: nudge
    # eight numbers, then pay for a single reload. The quick reload puts the camera back
    # where it was, so consecutive iterations frame the identical shot.
    #
    # ⚠ THE OBJ IS BUILD OUTPUT. "Log DSL" is the point of the tool; the file it writes
    # is scratch that the next `build_objs.py build --write` will overwrite. All tuning
    # belongs in src/lights/lights.layout.phdsl.

    # Slot layout AFTER each command's own prefix. LIGHT_PARAM's prefix is the light
    # class name; LIGHT_SPILL_CUSTOM has none and carries a trailing dataref token
    # instead. The twelve numbers line up either way, which is what lets one editor
    # drive both - only slot 6 differs in meaning (rheostat index vs. baked alpha) and
    # nothing here touches it.
    TUNER_SLOTS = (
        ("X",     0,  "pos"),
        ("Y",     1,  "pos"),
        ("Z",     2,  "pos"),
        ("Dir X", 8,  "dir"),
        ("Dir Y", 9,  "dir"),
        ("Dir Z", 10, "dir"),
        ("Size",  7,  "size"),
        ("Cone",  11, "cone"),
    )
    TUNER_NUM_COUNT = 12

    @staticmethod
    def _fmt_num(v):
        """3 dp with trailing zeros stripped - the same shape build_objs.fmt emits, so a
        tuner-written line is textually indistinguishable from a generated one."""
        s = "%.3f" % v
        if "." in s:
            s = s.rstrip("0").rstrip(".")
        return "0" if s in ("", "-0") else s

    def _aircraft_dir_raw(self):
        """The aircraft folder with its real casing (unlike _current_aircraft_dir, whose
        normcase output is for COMPARING paths, not opening files)."""
        try:
            fileName, path = xp.getNthAircraftModel(0)
            if path:
                return os.path.dirname(os.path.abspath(path))
        except Exception:
            _log("could not resolve aircraft folder:\n" + traceback.format_exc())
        return None

    def _tuner_load(self):
        """(Re)parse the installed lights_inn.obj into an editable light list."""
        self.tunerOrder, self.tunerLights, self.tunerVals = [], {}, {}
        self.tunerLines, self.tunerPath = [], None
        acf = self._aircraft_dir_raw()
        if not acf:
            self.tunerNote = "no aircraft loaded"
            return
        path = os.path.join(acf, TunerObjRelPath)
        if not os.path.isfile(path):
            self.tunerNote = "not found: %s" % TunerObjRelPath
            _log("tuner: %s does not exist (interior mod not installed?)" % path)
            return
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                self.tunerLines = f.read().split("\n")
        except Exception:
            self.tunerNote = "read failed"
            _log("tuner read failed:\n" + traceback.format_exc())
            return

        fixture, ordinal = "?", 0
        for idx, raw in enumerate(self.tunerLines):
            s = raw.strip()
            if not s:
                continue
            if s.startswith("#"):
                # The generator labels each gated block "# <Fixture> -> <Branch>", and
                # that comment - NOT ANIM_begin - is what starts a new light block.
                # Mounted fixtures (the panel floods) nest several ANIM_begin levels for
                # their ckpt/lights/<n> transform stack, so resetting on ANIM_begin
                # would restart the ordinal partway through a fixture.
                if "->" in s:
                    fixture = s.lstrip("#").split("->")[0].strip()
                    ordinal = 0
                continue
            toks = s.split()
            cmd = toks[0]
            if cmd == "LIGHT_PARAM":
                prefix, numStart = toks[1:2], 2
            elif cmd == "LIGHT_SPILL_CUSTOM":
                prefix, numStart = [], 1
            else:
                continue
            nums = toks[numStart:numStart + self.TUNER_NUM_COUNT]
            if len(nums) < self.TUNER_NUM_COUNT:
                continue
            try:
                nums = [float(n) for n in nums]
            except ValueError:
                continue
            tail = toks[numStart + self.TUNER_NUM_COUNT:]
            key = (fixture, ordinal)
            ordinal += 1
            entry = self.tunerLights.get(key)
            if entry is None:
                entry = {"cls": prefix[0] if prefix else "SPILL_CUSTOM", "refs": []}
                self.tunerLights[key] = entry
                self.tunerOrder.append(key)
                # The working copy comes from the FIRST branch. All branches share
                # geometry and differ only in color, so any one of them is right.
                self.tunerVals[key] = list(nums)
            # Keep each line's own leading whitespace: mounted fixtures sit several
            # ANIM_trans levels deep, so a fixed indent would silently reflow them.
            indent = raw[:len(raw) - len(raw.lstrip())]
            entry["refs"].append((idx, indent, cmd, prefix, list(nums), tail))

        self.tunerPath = path
        self.tunerSel = min(self.tunerSel, max(0, len(self.tunerOrder) - 1))
        self.tunerNote = "%d lights, %d branches" % (
            len(self.tunerOrder),
            len(self.tunerLights[self.tunerOrder[0]]["refs"]) if self.tunerOrder else 0)
        _log("tuner loaded %s (%s)" % (path, self.tunerNote))

    def _tuner_key(self):
        if not self.tunerOrder:
            return None
        return self.tunerOrder[max(0, min(self.tunerSel, len(self.tunerOrder) - 1))]

    def _tuner_select(self, delta):
        if self.tunerOrder:
            self.tunerSel = (self.tunerSel + delta) % len(self.tunerOrder)

    def _tuner_nudge(self, slot, kind, sign):
        key = self._tuner_key()
        if key is None:
            return
        step = (TunerCoarseStep if self.tunerCoarse else TunerFineStep)[kind]
        self.tunerVals[key][slot] += sign * step
        self.tunerNote = "edited (not applied)"

    def _tuner_revert(self):
        key = self._tuner_key()
        if key is None:
            return
        self.tunerVals[key] = list(self.tunerLights[key]["refs"][0][4])
        self.tunerNote = "reverted %s" % (key[0],)

    def _tuner_render(self, indent, cmd, prefix, nums, tail):
        parts = prefix + [self._fmt_num(n) for n in nums] + tail
        return indent + cmd + "\t" + " ".join(parts)

    def _tuner_apply(self, reload_after=True):
        """Write every staged edit into the OBJ, then reload so X-Plane re-reads it."""
        if not self.tunerPath or not self.tunerOrder:
            self.tunerNote = "nothing loaded"
            return
        edited = 0
        lines = list(self.tunerLines)
        for key in self.tunerOrder:
            vals = self.tunerVals[key]
            base = self.tunerLights[key]["refs"][0][4]
            if all(abs(vals[i] - base[i]) < 1e-9 for _, i, _ in self.TUNER_SLOTS):
                continue
            for idx, indent, cmd, prefix, nums, tail in self.tunerLights[key]["refs"]:
                out = list(nums)
                # Only the geometric slots are copied across; r/g/b and slot 6 stay as
                # each branch had them, so applying an edit never flattens the three
                # color variants into one.
                for _, slot, _ in self.TUNER_SLOTS:
                    out[slot] = vals[slot]
                lines[idx] = self._tuner_render(indent, cmd, prefix, out, tail)
                edited += 1
        if not edited:
            self.tunerNote = "no changes to apply"
            return
        try:
            # temp + os.replace: the same atomic-write discipline build_objs uses, so a
            # reload can never race a half-written OBJ (that combination crashed the sim
            # once already - see docs/dev-tools.md).
            tmp = self.tunerPath + ".photon-tuner.tmp"
            with open(tmp, "w", encoding="utf-8", newline="") as f:
                f.write("\n".join(lines))
            os.replace(tmp, self.tunerPath)
        except Exception:
            self.tunerNote = "WRITE FAILED - see log"
            _log("tuner write failed:\n" + traceback.format_exc())
            return
        _log("tuner wrote %d light lines to %s" % (edited, self.tunerPath))
        self.tunerNote = "applied %d lines" % edited
        self._tuner_log_dsl()
        if reload_after:
            self.Trigger()

    def _tuner_log_dsl(self):
        """Print the selected light as a DSL fragment, ready to paste into
        src/lights/lights.layout.phdsl - the actual deliverable of a tuning session."""
        key = self._tuner_key()
        if key is None:
            return
        v = self.tunerVals[key]
        fmt = self._fmt_num
        _log("---- DSL for %s (light %d) ----" % (key[0], key[1] + 1))
        _log("  pos: %s %s %s;  dir: %s %s %s;" %
             (fmt(v[0]), fmt(v[1]), fmt(v[2]), fmt(v[8]), fmt(v[9]), fmt(v[10])))
        _log("  size: %s;  cone: %s;   (light type %s)" %
             (fmt(v[7]), fmt(v[11]), self.tunerLights[key]["cls"]))
        _log("-------------------------------")

    def TunerDraw(self, windowID, refCon):
        try:
            l, t, r, b = xp.getWindowGeometry(windowID)
            mx, my = xp.getMouseLocationGlobal()
            fh = _font_height()
            lineH, btnH = fh + 8, fh + 10
            self.tunerHits = []
            key = self._tuner_key()

            y = t - UI_TOP_INSET
            title = "%s  (light %d of %d)" % (key[0], self.tunerSel + 1, len(self.tunerOrder)) \
                    if key else "no lights loaded"
            _draw_text(COL_BRIGHT, l + UI_PAD, y, title)
            y -= lineH
            _draw_text(COL_DIM, l + UI_PAD, y, self.tunerNote)
            y -= lineH + UI_GAP

            self._ui_button_row(l + UI_PAD, y - btnH, y, mx, my, self.tunerHits, [
                ("< Prev", lambda: self._tuner_select(-1)),
                ("Next >", lambda: self._tuner_select(+1)),
                ("Fine", lambda: setattr(self, "tunerCoarse", False), not self.tunerCoarse),
                ("Coarse", lambda: setattr(self, "tunerCoarse", True), self.tunerCoarse),
            ])
            y -= btnH + UI_GAP * 2

            if key is not None:
                vals = self.tunerVals[key]
                base = self.tunerLights[key]["refs"][0][4]
                for label, slot, kind in self.TUNER_SLOTS:
                    _draw_text(COL_DIM, l + UI_PAD, y - btnH + 4, label)
                    changed = abs(vals[slot] - base[slot]) > 1e-9
                    _draw_text(COL_ACTIVE if changed else COL_BRIGHT,
                               l + UI_PAD + 52, y - btnH + 4, self._fmt_num(vals[slot]))
                    self._ui_button_row(
                        l + UI_PAD + 120, y - btnH, y, mx, my, self.tunerHits,
                        [("-", (lambda s=slot, k=kind: self._tuner_nudge(s, k, -1))),
                         ("+", (lambda s=slot, k=kind: self._tuner_nudge(s, k, +1)))],
                        minWidth=34)
                    y -= btnH + 4
                y -= UI_GAP

            self._ui_button_row(l + UI_PAD, y - btnH, y, mx, my, self.tunerHits, [
                ("Apply & Reload", self._tuner_apply),
                ("Revert", self._tuner_revert),
                ("Log DSL", self._tuner_log_dsl),
                ("Rescan", self._tuner_load),
            ])
        except Exception:
            _log("TunerDraw error:\n" + traceback.format_exc())

    def TunerClick(self, windowID, x, y, inMouse, refCon):
        return self._ui_click(self.tunerHits, x, y, inMouse)

    def ShowTunerWindow(self):
        if not self.tunerOrder:
            self._tuner_load()
        if self.tunerWin is not None:
            xp.setWindowIsVisible(self.tunerWin, 1)
            xp.bringWindowToFront(self.tunerWin)
            return
        fh = _font_height()
        height = (UI_TOP_INSET + 2 * (fh + 8) + UI_GAP
                  + (fh + 10) + UI_GAP * 3
                  + len(self.TUNER_SLOTS) * (fh + 14)
                  + (fh + 10) + UI_PAD * 3)
        self.tunerWin = self._create_window("Photon Dev - Cockpit Light Tuner",
                                            400, height, self.TunerDraw, self.TunerClick)

    # ========================= live light debug =====================================
    # See the DebugManifestRelPath block up top for what this is. Registration happens
    # in XPluginStart because an OBJ resolves dataref NAMES as it loads, and a name that
    # does not exist yet reads 0 for the life of that aircraft.

    def _dbg_register_accessors(self):
        """Claim the whole ToLissPhoton/debug/light/<n> pool, once, at plugin start.

        Fixed size rather than sized from the manifest: at XPluginStart there is no
        aircraft, so there is no manifest. Slots past the end of the current one read
        as a zeroed light, which draws nothing."""
        for n in range(DebugMaxLights):
            try:
                # readRefCon, NOT refCon — XPPython3 has a separate refCon per
                # direction. `refCon=` raises TypeError and registers nothing.
                ref = xp.registerDataAccessor(DebugDataRefPrefix + str(n),
                                              readFloatArray=self.DbgReadArray,
                                              readRefCon=n)
                self.dbgAccessors.append(ref)
            except Exception:
                _log("could not register %s%d:\n%s"
                     % (DebugDataRefPrefix, n, traceback.format_exc()))
                break
        try:
            self.dbgPosAccessor = xp.registerDataAccessor(
                DebugPosDataRef, readFloatArray=self.DbgReadPos, readRefCon=0)
        except Exception:
            _log("could not register %s:\n%s"
                 % (DebugPosDataRef, traceback.format_exc()))
        try:
            # BOTH int and float readers. ANIM_hide reads its dataref as a FLOAT;
            # with only readInt registered X-Plane advertises the type wrong, the gate
            # reads 0, and the A/B toggle sticks on "original".
            self.dbgCompareAccessor = xp.registerDataAccessor(
                DebugCompareDataRef,
                readInt=lambda _r: int(self.dbgCompare),
                readFloat=lambda _r: float(self.dbgCompare),
                readDouble=lambda _r: float(self.dbgCompare))
        except Exception:
            _log("could not register %s:\n%s"
                 % (DebugCompareDataRef, traceback.format_exc()))
        self._dbg_register_marker_accessors()
        _log("live light debug: %d dataref slots registered%s"
             % (len(self.dbgAccessors),
                " + position array" if self.dbgPosAccessor else " (NO position array)"))
        self._dbg_publish_to_dataref_tools()

    def _dbg_register_marker_accessors(self):
        """The position-marker pool: one offset array, one show flag, two param sets.

        Four registrations for every light there will ever be, because the marker for
        light n is picked out by the VALUE of the show flag rather than by having its
        own dataref — the OBJ gates each light's block on `-0.5..n+0.5` plus
        `n+1.5..big`, the same paired-ANIM_hide encoding the DSL uses for a ternary
        category. -1 escapes both ranges and so shows every light at once."""
        try:
            self.dbgMarkerAccessors.append(xp.registerDataAccessor(
                DebugMarkerDataRef, readFloatArray=self.DbgReadMarker, readRefCon=0))
        except Exception:
            _log("could not register %s:\n%s"
                 % (DebugMarkerDataRef, traceback.format_exc()))
        try:
            # int AND float, for the same reason the compare flag needs both: the OBJ
            # gates on this with ANIM_hide, which reads its dataref as a float.
            self.dbgMarkerAccessors.append(xp.registerDataAccessor(
                DebugMarkerShowDataRef,
                readInt=lambda _r: int(self._dbg_marker_value()),
                readFloat=lambda _r: float(self._dbg_marker_value()),
                readDouble=lambda _r: float(self._dbg_marker_value())))
        except Exception:
            _log("could not register %s:\n%s"
                 % (DebugMarkerShowDataRef, traceback.format_exc()))
        for kind in range(DebugMarkerRoles):
            try:
                self.dbgMarkerAccessors.append(xp.registerDataAccessor(
                    "%s/%d" % (DebugMarkerParamDataRef, kind),
                    readFloatArray=self.DbgReadMarkerParam, readRefCon=kind))
            except Exception:
                _log("could not register %s/%d:\n%s"
                     % (DebugMarkerParamDataRef, kind, traceback.format_exc()))

    def DbgReadMarker(self, refCon, values, offset, count):
        """Per-dot XYZ offsets, `DebugMaxLights * DebugMarkerDots * 3` floats.

        Sized from the CONSTANT, not the manifest, for the same reason the light pool
        is: registration happens at XPluginStart when no aircraft exists."""
        try:
            total = DebugMaxLights * DebugMarkerDots * DebugPosCount
            if values is None:
                return total
            if offset < 0:
                offset = 0
            if count < 0:
                count = total
            # Stride comes from the MANIFEST, not the constant: the installed OBJ baked
            # its own dot count into the indices, so an older one must still address its
            # own dots rather than a neighbouring light's.
            stride = self.dbgMarkerDots or DebugMarkerDots
            out = []
            for i in range(offset, min(total, offset + count)):
                dot, axis = divmod(i, DebugPosCount)
                n, dot = divmod(dot, stride)
                out.append(self._dbg_marker_offset(n, dot)[axis])
            values.extend(out)
            return len(out)
        except Exception:
            _log("DbgReadMarker error:\n" + traceback.format_exc())
            return 0

    def DbgReadMarkerParam(self, refCon, values, offset, count):
        """The nine LIGHT_CUSTOM params for a marker dot: r g b a s s1 t1 s2 t2.

        Writes ALL NINE rather than modifying in place. Whether X-Plane pre-fills a
        custom light's buffer with the OBJ's baked values is still an open question on
        this project (interior_plan.md), and a marker that draws black because of it
        would be read as the marker feature not working."""
        try:
            if values is None:
                return DebugParamCount
            if offset < 0:
                offset = 0
            if count < 0:                       # the "give me all of it" convention
                count = DebugParamCount
            colour = (DebugMarkerOriginColor, DebugMarkerAxisColor,
                      DebugMarkerRimColor)[max(0, min(2, int(refCon)))]
            params = list(colour) + [self.dbgMarkerSize] + list(DebugMarkerST)
            out = params[offset:min(DebugParamCount, offset + count)]
            values.extend(out)
            return len(out)
        except Exception:
            _log("DbgReadMarkerParam error:\n" + traceback.format_exc())
            return 0

    def _dbg_marker_offset(self, n, dot):
        """Where marker dot `dot` of light `n` sits, as an offset from the baked position.

        Three parts, and the split must match `_debug_marker_role`:

        * dot 0 — the light's own origin.
        * dots 1..AxisDots — evenly along the aim out to `dbgMarkerReach`.
        * the rest — a ring on the CONE RIM at that same distance, radius
          `tan(half-spread) * reach`. This is the part that makes aim judgeable: a
          bare axis line was reported in-sim as not obviously following the light,
          which is what a short line looks like beside a pool of light a metre wide.
          The ring draws the beam the `spread` row claims you asked for, so "is the
          lit patch inside the ring" is a question with a yes/no answer.

        ⚠ AIM COMES FROM THE AUTHORED VECTOR (slots 5..7), NOT from `_dbg_params`, so
        it does NOT carry the orientation hypothesis. This is the opposite of what the
        first version did, and the reversal is the point:

        `_dbg_params` is where the orientation cycler is applied, so a marker that read
        through it moved the arrow and the light TOGETHER — which made the one
        instrument that could expose an axis mismatch structurally incapable of showing
        one. Reported in-sim 2026-07-30: with yaw 180, editing PITCH moved the light
        horizontally while the markers moved vertically as asked. Splitting them is
        what turns `Dir < / >` into a usable search: the marker is the reference (what
        the angles say), the light is the sim's answer, and you cycle until they agree.

        Normalised here whatever the vector's length, because the marker shows
        direction — length is the sim's problem, and `build` warns about it."""
        base = self._dbg_pos_offset(n)
        if dot == 0 or n >= len(self.dbgLights):
            return base
        # While probing, the marker draws the PROBE's own direction slots — so the arrow
        # shows what slot 5/6/7 was set to and the light shows what X-Plane did with it.
        # That side-by-side IS the measurement.
        vals = self._dbg_probe_params(n) or self._dbg_values(n) or []
        if len(vals) < DebugParamCount:
            return base
        d = vals[5:8]
        if math.sqrt(sum(v * v for v in d)) < 1e-9:
            return base          # omnidirectional: no aim to draw, stack them on the dot
        axis, u, w = _perp_basis(d)
        reach = self.dbgMarkerReach
        if dot <= DebugMarkerAxisDots:
            out = [axis[i] * reach * dot / DebugMarkerAxisDots
                   for i in range(DebugPosCount)]
        else:
            # Half-spread from the cone cosine, clamped just under 90 deg: at 90 the
            # tangent runs away and the ring would fly off to the horizon.
            half = min(_cone_to_spread(vals[8]), 89.0)
            radius = math.tan(math.radians(half)) * reach
            ang = 2.0 * math.pi * (dot - DebugMarkerAxisDots - 1) / DebugMarkerRimDots
            out = [axis[i] * reach
                   + (u[i] * math.cos(ang) + w[i] * math.sin(ang)) * radius
                   for i in range(DebugPosCount)]
        return [max(-DebugPosRange, min(DebugPosRange, base[i] + out[i]))
                for i in range(DebugPosCount)]

    def _dbg_publish_to_dataref_tools(self):
        """Tell DataRefTool / DataRefEditor these datarefs exist.

        Custom datarefs are not discoverable: those tools scan X-Plane's own table plus
        whatever plugins announce, so an unannounced accessor works perfectly while
        appearing nowhere. Message 0x01000000, name as payload."""
        names = [DebugDataRefPrefix + str(n) for n in range(len(self.dbgAccessors))]
        if self.dbgPosAccessor is not None:
            names.append(DebugPosDataRef)
        if self.dbgCompareAccessor is not None:
            names.append(DebugCompareDataRef)
        if self.dbgMarkerAccessors:
            names += [DebugMarkerDataRef, DebugMarkerShowDataRef]
            names += ["%s/%d" % (DebugMarkerParamDataRef, k)
                      for k in range(DebugMarkerRoles)]
        found = []
        for sig in ("com.leecbaker.datareftool", "xplanesdk.examples.DataRefEditor"):
            try:
                pid = xp.findPluginBySignature(sig)
            except Exception:
                continue
            if pid is not None and pid != xp.NO_PLUGIN_ID:
                found.append(sig)
                for name in names:
                    try:
                        xp.sendMessageToPlugin(pid, 0x01000000, name)
                    except Exception:
                        break
        _log("live light debug: announced to %s"
             % (", ".join(found) if found else "no dataref browser (none loaded)"))

    def _dbg_unregister_accessors(self):
        for ref in self.dbgAccessors + self.dbgMarkerAccessors:
            try:
                xp.unregisterDataAccessor(ref)
            except Exception:
                pass
        self.dbgAccessors = []
        self.dbgMarkerAccessors = []
        for attr in ("dbgPosAccessor", "dbgCompareAccessor"):
            ref = getattr(self, attr, None)
            if ref is not None:
                try:
                    xp.unregisterDataAccessor(ref)
                except Exception:
                    pass
                setattr(self, attr, None)

    def DbgReadArray(self, refCon, values, offset, count):
        """X-Plane pulling one debug light's nine params, once per frame per light.

        Writes ALL nine slots, never a modify-in-place of a pre-filled buffer: whether
        X-Plane pre-fills one is an open question, and here the plugin owns every param
        anyway.

        ⚠ REORDERS into the dataref's own layout on the way out — `r g b a size SEMI dx
        dy dz`, cone FIRST in the trailing four. See DebugDataRefOrder for the
        measurement. This is the ONE place the translation happens; everything upstream
        works in the readable order.

        Two XPPython3 conventions, both of which fail silently: `values` is an EMPTY
        list to `.extend()`, not a buffer to index; and `count == -1` means "all of
        them", not "none"."""
        try:
            if values is None:
                return DebugParamCount             # size query
            params = self._dbg_wire_params(refCon)
            if offset < 0:
                offset = 0
            if count < 0:
                count = DebugParamCount
            out = params[offset:min(DebugParamCount, offset + count)]
            values.extend(out)
            return len(out)
        except Exception:
            _log("DbgReadArray error:\n" + traceback.format_exc())
            return 0

    def DbgReadPos(self, refCon, values, offset, count):
        """The shared position-offset array, `3 * DebugMaxLights` floats.

        Read one element at a time in practice — each `ANIM_trans` references
        `pos[3n+axis]`. Same list conventions as DbgReadArray."""
        try:
            total = DebugMaxLights * DebugPosCount
            if values is None:
                return total
            if offset < 0:
                offset = 0
            if count < 0:
                count = total
            out = []
            for i in range(offset, min(total, offset + count)):
                n, axis = divmod(i, DebugPosCount)
                out.append(self._dbg_pos_offset(n)[axis])
            values.extend(out)
            return len(out)
        except Exception:
            _log("DbgReadPos error:\n" + traceback.format_exc())
            return 0

    def _dbg_pos_offset(self, n):
        """What the three ANIM_trans blocks for light `n` should read, in metres.

        Two terms, kept separate so the orientation search stays usable:

        * The ORIENTATION CORRECTION, `permute(pos) - pos` — the vector that relocates
          the light from where the OBJ baked it to where the hypothesis says it belongs.
          Zero at the identity, which is why nothing moves until you cycle.
        * The MANUAL nudge, always in plain OBJ axes and deliberately NOT permuted, so
          "X +" still moves along the axis the readout calls X.

        Clamped per component because X-Plane clamps outside the outermost keyframe
        anyway, and the window should agree with the sim about what was applied."""
        vals = self._dbg_values(n)
        # With no wrappers in the OBJ nothing reads this array, so a non-zero offset
        # would only make the window disagree with the sim.
        if not vals or not self.dbgPosTunable:
            return [0.0, 0.0, 0.0]
        baked = list(self.dbgLights[n].get("pos", [0, 0, 0])) + [0.0, 0.0, 0.0]
        baked = baked[:3]
        moved = _orient(self.dbgOrientPos, baked)
        return [max(-DebugPosRange,
                    min(DebugPosRange,
                        moved[i] - baked[i] + vals[DebugPosSlot + i]))
                for i in range(DebugPosCount)]

    def _dbg_values(self, n):
        """Working values for light `n`, loading the manifest on first use.

        The lazy load matters: accessors are registered at XPluginStart, long before any
        aircraft exists, and the window that used to trigger the read might never be
        opened — which left every light reading zeros from the first frame. Loading on
        first ACCESS happens after the OBJ loads and before anything is drawn.

        `dbgLoadTried` keeps it to one attempt per aircraft, so a missing manifest (the
        normal case) costs one stat(), not one per light per frame."""
        if not self.dbgVals and not self.dbgLoadTried:
            self.dbgLoadTried = True
            self._dbg_load()
        return self.dbgVals[n] if n < len(self.dbgVals) else None

    def _dbg_rheostat(self, source, index):
        """The 0..1 brightness knob a light rides, cached for the current frame.

        Called once per light per frame, hence the cache. The array variants go through
        getDatavf and ckpt/lights/map as a type-detected scalar: X-Plane returns 0 on a
        type mismatch rather than erroring, which is how a light goes silently black."""
        now = xp.getElapsedTime()
        if now != self.dbgFrameTime:
            self.dbgFrameTime = now
            self.dbgFrameVals = {}
        key = (source, index)
        if key in self.dbgFrameVals:
            return self.dbgFrameVals[key]
        name = {"panel": DebugPanelRheostat,
                "inst": DebugInstRheostat,
                "du": DebugDUBrightness}.get(source, DebugMapRheostat)
        if name not in self.dbgRheostatRefs:
            self.dbgRheostatRefs[name] = xp.findDataRef(name)
            if self.dbgRheostatRefs[name] is None:
                _log("live light debug: rheostat %s NOT FOUND — those lights will "
                     "sit at full brightness" % name)
        ref = self.dbgRheostatRefs[name]
        value = 1.0
        if ref is not None:
            try:
                if source in ("panel", "inst", "du"):
                    out = []
                    xp.getDatavf(ref, out, index, 1)
                    value = out[0] if out else 0.0
                else:
                    types = xp.getDataRefTypes(ref)
                    if types & TYPE_INT:
                        value = float(xp.getDatai(ref))
                    elif types & TYPE_DOUBLE:
                        value = float(xp.getDatad(ref))
                    else:
                        value = float(xp.getDataf(ref))
            except Exception:
                value = 0.0
        value = max(0.0, min(1.0, value))
        self.dbgFrameVals[key] = value
        return value

    def _dbg_wire_params(self, n):
        """`_dbg_params` permuted into the order the dataref actually wants.

        Offsets in DbgReadArray index THIS array, not the readable one, so the reorder
        has to happen before the slice — X-Plane asking for [5:9] must get semi/dx/dy/dz,
        not dx/dy/dz/cone."""
        params = self._dbg_params(n)
        return [params[i] for i in DebugDataRefOrder]

    def _dbg_probe_params(self, n):
        """The nine floats a slot probe sends, or None when not probing.

        Deliberately bypasses EVERYTHING else — orientation, rheostat, Isolate, Blink,
        Mark. A probe is only worth anything if the values reaching X-Plane are exactly
        the ones on screen, and each of those would otherwise be a second variable in an
        experiment designed to have one. Every other light is blacked out for the same
        reason: two lit lights and you cannot say which one moved."""
        if self.dbgProbe is None:
            return None
        if n != self.dbgSel:
            return [0.0] * DebugParamCount
        v = list(DebugProbeBaseline)
        v[self.dbgProbe] = DebugProbeValue
        return v

    def _dbg_params(self, n):
        """The nine spill floats slot `n` currently reads."""
        probe = self._dbg_probe_params(n)
        if probe is not None:
            return probe
        vals = self._dbg_values(n)
        if vals is None:
            return [0.0] * DebugParamCount          # past the manifest: draws nothing
        v = list(vals[:DebugParamCount])
        # Aim goes through the orientation hypothesis on its way out, so one click
        # re-aims every light at once. Slots 5..7 are dx/dy/dz.
        v[5], v[6], v[7] = _orient(self.dbgOrientDir, v[5:8])
        light = self.dbgLights[n]
        selected = (n == self.dbgSel)
        if self.dbgMark and selected:
            v[0], v[1], v[2] = DebugMarkColor
        alpha = v[3] * self._dbg_rheostat(light.get("source", "panel"),
                                          int(light.get("index", 0)))
        if self.dbgIsolate and not selected:
            alpha = 0.0
        if self.dbgBlink and selected:
            # Square wave, not a fade: a hard on/off is easier to pick out of a cockpit
            # where several lights already glow at similar levels.
            alpha *= 1.0 if (xp.getElapsedTime() * DebugBlinkHz) % 1.0 < 0.5 else 0.0
        v[3] = alpha
        return v

    def _dbg_load(self):
        """(Re)read the newest debug manifest from the installed aircraft.

        There is one manifest per debug-capable target (cockpit, screens) and both
        number their dataref slots from 0, so driving two at once would have every
        screen light fighting a cockpit light for the same slot. The most recently
        WRITTEN manifest wins — that is the build you just ran — and its name goes in
        the window so "why is the list full of the wrong lights" is never a puzzle."""
        self.dbgLights, self.dbgVals, self.dbgBase = [], [], []
        self.dbgPosTunable = False      # re-asserted from the manifest below
        self.dbgMarkerDots = 0          # 0 = this OBJ has no markers in it
        self.dbgTarget = ""
        acf = self._aircraft_dir_raw()
        if not acf:
            self.dbgNote = "no aircraft loaded"
            return
        found = []
        for label, rel in DebugManifests:
            p = os.path.join(acf, rel)
            if os.path.exists(p):
                try:
                    found.append((os.path.getmtime(p), label, p))
                except OSError:
                    pass
        if not found:
            self.dbgNote = "no debug OBJ installed"
            _log("live light debug: none of %s found under %s — build with "
                 "`build_objs.py build --target interior|screens --debug --write`"
                 % (", ".join(r for _, r in DebugManifests), acf))
            return
        found.sort(reverse=True)        # newest mtime first
        _, self.dbgTarget, path = found[0]
        if len(found) > 1:
            _log("live light debug: %d debug OBJs are installed (%s); driving the "
                 "newest, %s. They share the same dataref slots, so rebuild the "
                 "other WITHOUT --debug to avoid two lights per slot."
                 % (len(found), ", ".join(f[1] for f in found), self.dbgTarget))
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            self.dbgLights = list(data.get("lights", []))
            # Default False: a manifest is always written with its OBJ, so a missing key
            # means an old or hand-made one, and hiding the controls is the safe read.
            self.dbgPosTunable = bool(data.get("pos_tunable", False))
            # How many marker dots the OBJ baked per light. It strides the offset
            # array, so reading it from the manifest rather than assuming the constant
            # means a stale OBJ drives ITS OWN dots instead of a neighbour's.
            self.dbgMarkerDots = int(data.get("marker_dots", 0))
        except Exception:
            self.dbgNote = "manifest read failed"
            _log("live light debug read failed:\n" + traceback.format_exc())
            return
        for light in self.dbgLights:
            rgb = list(light.get("rgb", [1, 1, 1]))
            d = list(light.get("dir", [0, 0, 0]))
            # Seeded from the manifest — the OBJ's own authored values — so the window
            # opens on the real light. The x/y/z tail is an OFFSET and starts at zero;
            # the baked position it applies to is light["pos"].
            d = [float(x) for x in (list(d) + [0.0, 0.0, 0.0])[:3]]
            # Aim seeded FROM the authored vector, then the vector rewritten from the
            # aim, so the window opens on the light as authored and every later edit
            # flows angles -> vector. The rewrite also normalises: a light authored
            # with a non-unit dir is shown, and emitted, as the unit form of itself.
            aim = _dir_to_aim(d)
            vals = [float(rgb[0]), float(rgb[1]), float(rgb[2]), 1.0,
                    float(light.get("size", 1)), d[0], d[1], d[2],
                    float(light.get("cone", 1)), 0.0, 0.0, 0.0,
                    round(aim[0], 3), round(aim[1], 3)]
            self.dbgVals.append(vals)
            self._dbg_apply_aim(len(self.dbgVals) - 1)
            self.dbgBase.append(list(self.dbgVals[-1]))
        if len(self.dbgLights) > len(self.dbgAccessors):
            self.dbgNote = "TOO MANY LIGHTS (%d > %d slots)" % (
                len(self.dbgLights), len(self.dbgAccessors))
            _log("live light debug: manifest has %d lights but only %d dataref slots "
                 "are registered — raise DebugMaxLights here AND DEBUG_MAX_LIGHTS in "
                 "build/build_objs.py" % (len(self.dbgLights), len(self.dbgAccessors)))
            return
        self.dbgSel = min(self.dbgSel, max(0, len(self.dbgLights) - 1))
        self.dbgNote = "%s: %d lights" % (self.dbgTarget, len(self.dbgLights))
        _log("live light debug loaded %s (%d lights)" % (path, len(self.dbgLights)))

    # The X/Y/Z rows are OFFSETS from the baked position — the ANIM_trans wrappers
    # translate the light, they do not place it. The window shows the resulting
    # absolute position beside them.
    # Aim is edited as PITCH/YAW in degrees and spread as a HALF-ANGLE in degrees,
    # not as dx/dy/dz and a cosine. Three components whose length is not free, and a
    # cone number that runs backwards, are not quantities anyone can steer by hand —
    # reported in-sim as "very difficult to get things pointed correctly". Slots 5..7
    # and 8 still hold the vector and the cosine, because that is what the light
    # reads; these rows write them through _dbg_set_aim / the `spread` nudge.
    DEBUG_SLOTS = (
        ("Red",    0, "rgb"),
        ("Green",  1, "rgb"),
        ("Blue",   2, "rgb"),
        ("Alpha",  3, "alpha"),
        ("Size",   4, "size"),
        ("Pitch",  DebugAimSlot,     "pitch"),
        ("Yaw",    DebugAimSlot + 1, "yaw"),
        ("Spread", 8, "spread"),
    )

    # Shown only when the installed OBJ has the ANIM_trans wrappers, i.e. anything but a
    # --no-debug-pos build. Split out rather than filtered inline so the window height
    # can be computed from the rows that will really be drawn.
    DEBUG_POS_SLOTS = (
        ("X",     9,  "pos"),
        ("Y",     10, "pos"),
        ("Z",     11, "pos"),
    )

    def _dbg_slots(self):
        return self.DEBUG_SLOTS + (self.DEBUG_POS_SLOTS if self.dbgPosTunable else ())

    def _dbg_rescan(self):
        """Re-read the manifest on demand, re-arming the one-shot lazy load so one that
        has only just appeared is picked up.

        Also the sanctioned place to resize: this runs from a click, a safe context to
        destroy a window, whereas the lazy load runs from a dataref accessor."""
        self.dbgLoadTried = True
        self._dbg_load()
        self._dbg_resize_window()

    def _dbg_resize_window(self):
        """Rebuild the window if the visible row count changed under it.

        The X/Y/Z rows come and go with `--no-debug-pos` and the window has fixed
        resizing limits, so a stale height clips the action buttons off the bottom."""
        if self.dbgWin is None or self.dbgWinSlots == len(self._dbg_slots()):
            return
        self._destroy_window("dbgWin")
        self.ShowDebugWindow()

    def _dbg_select(self, delta):
        if self.dbgLights:
            self.dbgSel = (self.dbgSel + delta) % len(self.dbgLights)

    def _dbg_select_category(self, delta):
        """Jump to the first light of the next/previous CATEGORY — the fastest way from
        "the Map Lights row does nothing" to the lights that row drives."""
        if not self.dbgLights:
            return
        cats = []
        for light in self.dbgLights:
            if light.get("category") not in cats:
                cats.append(light.get("category"))
        cur = self.dbgLights[self.dbgSel].get("category")
        want = cats[(cats.index(cur) + delta) % len(cats)]
        for i, light in enumerate(self.dbgLights):
            if light.get("category") == want:
                self.dbgSel = i
                return

    def _dbg_nudge(self, slot, kind, sign):
        if not self.dbgVals:
            return
        v = self.dbgVals[self.dbgSel]
        if kind == "spread":
            # Edited in DEGREES, stored as the cosine the light reads. Clamped to
            # 0.1..89.9: at 0 the beam is a line and at 90 it is a hemisphere whose
            # rim tangent runs to infinity, and neither is a usable tuning state.
            deg = max(0.1, min(89.9, _cone_to_spread(v[8]) + sign * self.dbgStep))
            v[8] = _spread_to_cone(round(deg, 6))
            return
        # Rounded to the step ladder's own resolution: 0.1 added ten times is
        # 0.9999999999999999 in binary floating point, and a window full of values
        # like that is unreadable and gets pasted into the .phdsl as-is.
        v[slot] = round(v[slot] + sign * self.dbgStep, 6)
        if kind == "pitch":
            # Pitch is clamped, yaw wraps. Past +-90 pitch would start coming back
            # down while yaw silently flipped 180 — an aim that is impossible to
            # steer. Yaw has no such seam: 359 and -1 are the same heading.
            v[slot] = max(-90.0, min(90.0, v[slot]))
            self._dbg_apply_aim(self.dbgSel)
        elif kind == "yaw":
            v[slot] = (v[slot] + 180.0) % 360.0 - 180.0
            self._dbg_apply_aim(self.dbgSel)
        elif kind in ("rgb", "alpha"):
            v[slot] = max(0.0, min(1.0, v[slot]))
        elif kind == "size":
            v[slot] = max(0.0, v[slot])
        elif kind == "pos":
            # X-Plane clamps outside the ∓DebugPosRange keyframes, so a larger value
            # would move nothing while the window claimed it had.
            v[slot] = max(-DebugPosRange, min(DebugPosRange, v[slot]))

    def _dbg_marker_value(self):
        """The number the OBJ's ANIM_hide gate reads: 0 off, -1 all, n+1 = light n."""
        if self.dbgMarkerMode == "all":
            return DebugMarkerAll
        if self.dbgMarkerMode == "this":
            return self.dbgSel + 1
        return DebugMarkerOff

    def _dbg_set_marker(self, mode):
        self.dbgMarkerMode = mode
        if mode != "off" and not self.dbgMarkerDots:
            # The controls stay live so the state is not mysteriously unclickable, but
            # an OBJ built before markers existed has no dots to show.
            self.dbgNote = "this debug OBJ has no markers — rebuild it"

    def _dbg_nudge_marker(self, what, sign):
        delta = sign * self.dbgStep
        if what == "reach":
            self.dbgMarkerReach = max(0.05, min(DebugPosRange / 2.0,
                                                round(self.dbgMarkerReach + delta, 6)))
        else:
            self.dbgMarkerSize = max(0.005, round(self.dbgMarkerSize + delta, 6))

    def _dbg_set_probe(self, slot):
        """Enter/leave the slot probe. Logged, because the value of this control is being
        able to report what each slot did and that moment is not one for reading a
        window. Turns the markers on with it: the arrow is the reference the light is
        being compared against."""
        self.dbgProbe = slot
        if slot is None:
            self.dbgNote = "probe off — back to normal tuning"
        else:
            v = list(DebugProbeBaseline)
            v[slot] = DebugProbeValue
            self.dbgNote = ("probe slot %d = %s (expect: %s)"
                            % (slot, self._fmt_num(DebugProbeValue),
                               DebugProbeExpect[slot]))
            if self.dbgMarkerMode == "off":
                self.dbgMarkerMode = "this"
            _log("live light debug: %s" % self.dbgNote)
            _log("  working order: r=%s g=%s b=%s a=%s size=%s dx=%s dy=%s dz=%s cone=%s"
                 % tuple(self._fmt_num(x) for x in v))
            _log("  wire order:    %s"
                 % "  ".join("%s=%s" % (name, self._fmt_num(v[i])) for name, i in
                              zip(DebugWireNames, DebugDataRefOrder)))

    def _dbg_aim_preset(self, pitch, yaw, label):
        """Put the selected light on a cardinal axis, from a KNOWN state.

        Clears the dir orientation hypothesis as part of the click. That is deliberate,
        not a side effect: the point of a preset is that "I clicked Up" fully determines
        what was asked for, and a leftover permutation from the cycler would silently
        make it mean something else — which is indistinguishable in the cockpit from the
        sim mis-reading the axes, i.e. exactly the confusion this is meant to resolve."""
        if not self.dbgVals:
            return
        v = self.dbgVals[self.dbgSel]
        v[DebugAimSlot], v[DebugAimSlot + 1] = pitch, yaw
        self._dbg_apply_aim(self.dbgSel)
        cleared = " (dir orientation reset to identity)" if self.dbgOrientDir else ""
        self.dbgOrientDir = 0
        self.dbgNote = "aim %s: pitch %g yaw %g -> dir %s%s" % (
            label, pitch, yaw,
            " ".join(self._fmt_num(x) for x in v[5:8]), cleared)
        _log("live light debug: %s" % self.dbgNote)

    def _dbg_apply_aim(self, n):
        """Rewrite slots 5..7 (dx/dy/dz) from slots 12..13 (pitch/yaw).

        One direction of flow, always. The angles are what the window edits and what
        "Log DSL" prints; the vector is a derived value the light happens to read. That
        also means the tuner can only ever emit a UNIT vector, which is the class of
        bug that made the interior's cones inert (docs/dsl.md)."""
        if n >= len(self.dbgVals):
            return
        v = self.dbgVals[n]
        if v[5] == 0.0 and v[6] == 0.0 and v[7] == 0.0:
            return          # omnidirectional light: it has no aim to steer
        v[5], v[6], v[7] = _aim_to_dir(v[DebugAimSlot], v[DebugAimSlot + 1])

    def _dbg_cycle_orient(self, which, delta):
        """Step the orientation hypothesis. `which` is "pos", "dir" or "both".

        Logged every time: the value of this control is being able to report WHICH of
        the 48 looked right, and that moment is not one for reading a window."""
        n = len(DebugOrientations)
        # Link is meaningless without the position wrappers: a "Dir >" click would
        # silently advance a position hypothesis that nothing can apply.
        link = self.dbgOrientLink and self.dbgPosTunable
        if self.dbgPosTunable and (which in ("pos", "both") or link):
            self.dbgOrientPos = (self.dbgOrientPos + delta) % n
        if which in ("dir", "both") or link:
            self.dbgOrientDir = (self.dbgOrientDir + delta) % n
        self.dbgNote = "orient pos %s / dir %s" % (self._dbg_orient_text("pos"),
                                                   self._dbg_orient_text("dir"))
        _log("live light debug: %s" % self.dbgNote)

    def _dbg_orient_text(self, which):
        idx = self.dbgOrientPos if which == "pos" else self.dbgOrientDir
        return "%d/%d %s" % (idx + 1, len(DebugOrientations),
                             DebugOrientations[idx][2])

    def _dbg_reset_orient(self):
        self.dbgOrientPos = self.dbgOrientDir = 0
        self.dbgNote = "orientation back to identity (X Y Z)"

    def _dbg_toggle_compare(self):
        self.dbgCompare = 0 if self.dbgCompare else 1
        self.dbgNote = ("showing ORIGINAL light lines (tuner has no effect)"
                        if not self.dbgCompare else "showing tunable debug lights")
        _log("live light debug: %s" % self.dbgNote)

    def _dbg_revert(self, everything=False):
        if not self.dbgVals:
            return
        targets = range(len(self.dbgVals)) if everything else (self.dbgSel,)
        for i in targets:
            self.dbgVals[i] = list(self.dbgBase[i])
        self.dbgNote = "reverted all" if everything else "reverted"

    def _dbg_log_dsl(self):
        """Dump the selected light in .phdsl shape — the deliverable of a session.

        Split into the two files it belongs to, because color and geometry live apart
        in this DSL: a palette entry carries the rgb, the light type or fixture carries
        aim/cone/size. Pasting rgb into a light type would hard-code a color that is
        supposed to change with the profile."""
        if not self.dbgLights:
            return
        light = self.dbgLights[self.dbgSel]
        v = self.dbgVals[self.dbgSel]
        fmt = self._fmt_num
        _log("---- DSL for %s (light %d of %d) ----"
             % (light.get("name"), self.dbgSel + 1, len(self.dbgLights)))
        _log("  fixture %s, category %s, %s[%s]%s"
             % (light.get("fixture"), light.get("category"), light.get("source"),
                light.get("index"),
                ", mount %s" % light["mount"] if light.get("mount") else ""))
        pos = self._dbg_abs_pos(self.dbgSel)
        _log("  # lights.layout.phdsl — position is ABSOLUTE here (baked + your offset):")
        # `aim:` is what the DSL should carry — it is what was actually steered, it
        # cannot express a non-unit vector, and "10 degrees further down" is an edit
        # someone can make to it later. The vector is logged beside it as a comment
        # so a diff against a pre-`aim` .phdsl is still readable.
        _log("  %s { pos: %s %s %s;  aim: %s %s;  spread: %s; }"
             % (light.get("name"), fmt(pos[0]), fmt(pos[1]), fmt(pos[2]),
                fmt(v[DebugAimSlot]), fmt(v[DebugAimSlot + 1]),
                fmt(round(_cone_to_spread(v[8]), 3))))
        _log("    # same thing as vectors: dir: %s %s %s;  cone: %s;"
             % (fmt(v[5]), fmt(v[6]), fmt(v[7]), fmt(v[8])))
        if light.get("mount"):
            _log("    ^ MOUNT-LOCAL: this light rides the %s transform stack, so that "
                 "pos is relative to it, exactly as the .phdsl fixture writes it."
                 % light["mount"])
        _log("  # lights.style.phdsl — the light TYPE carries these:")
        _log("  size: %s;  spread: %s;   // spread is the half-angle; cone: %s is "
             "the same thing as a cosine"
             % (fmt(v[4]), fmt(round(_cone_to_spread(v[8]), 3)), fmt(v[8])))
        _log("  # lights.style.phdsl — color belongs to a PALETTE entry, not the light:")
        _log("  <color-name>: %s %s %s;   (alpha %s is the tuner's own multiplier and "
             "has no .phdsl equivalent — it is the rheostat in the sim)"
             % (fmt(v[0]), fmt(v[1]), fmt(v[2]), fmt(v[3])))
        _log("  offset applied was %s %s %s from baked %s"
             % (fmt(v[9]), fmt(v[10]), fmt(v[11]),
                " ".join(fmt(x) for x in light.get("pos", []))))
        # The EXACT nine floats X-Plane reads for this light, in order, labelled. This
        # is the line that separates "our maths is wrong" from "the sim disagrees about
        # what slot 5 means" — the two are indistinguishable from inside the cockpit,
        # and that ambiguity is what the aim-axis report came down to.
        # In WIRE order, which is what X-Plane actually receives — cone first in the
        # trailing four. Logging the readable order here would have hidden the very bug
        # that made this line necessary.
        _log("  dataref %s/%d reads (wire order): %s"
             % (DebugDataRefPrefix.rstrip("/"), self.dbgSel,
                "  ".join("%s=%s" % (name, fmt(val)) for name, val in
                          zip(DebugWireNames, self._dbg_wire_params(self.dbgSel)))))
        if self.dbgOrientDir:
            _log("    ^ dx/dy/dz above are PERMUTED by orient dir %s; the authored aim "
                 "is %s %s %s (which is what the markers draw)"
                 % (self._dbg_orient_text("dir"), fmt(v[5]), fmt(v[6]), fmt(v[7])))
        # Always logged, even at the identity: a number copied out of a session with an
        # orientation applied means something different, and the numbers do not say so.
        _log("  orientation  pos %s   dir %s"
             % (self._dbg_orient_text("pos"), self._dbg_orient_text("dir")))
        _log("---------------------------------------")

    def _dbg_abs_pos(self, n):
        """Where light `n` actually ends up — what its `pos:` should become.

        Goes through _dbg_pos_offset rather than adding the manual nudge directly, so
        the number shown includes any orientation correction and its clamping."""
        light = self.dbgLights[n]
        baked = (list(light.get("pos", [0, 0, 0])) + [0.0, 0.0, 0.0])[:3]
        off = self._dbg_pos_offset(n)
        return [baked[i] + off[i] for i in range(DebugPosCount)]

    def DbgDraw(self, windowID, refCon):
        try:
            l, t, r, b = xp.getWindowGeometry(windowID)
            mx, my = xp.getMouseLocationGlobal()
            fh = _font_height()
            lineH, btnH = fh + 8, fh + 10
            self.dbgHits = []
            light = self.dbgLights[self.dbgSel] if self.dbgLights else None

            y = t - UI_TOP_INSET
            if light:
                title = "%s   (%d of %d)" % (light.get("name"), self.dbgSel + 1,
                                             len(self.dbgLights))
            else:
                title = "no debug lights loaded"
            _draw_text(COL_BRIGHT, l + UI_PAD, y, title)
            y -= lineH
            if light:
                sub = "%s / %s / %s[%s]%s" % (
                    light.get("fixture"), light.get("category"), light.get("source"),
                    light.get("index"),
                    "  mount %s" % light["mount"] if light.get("mount") else "")
            else:
                sub = self.dbgNote
            _draw_text(COL_DIM, l + UI_PAD, y, sub)
            y -= lineH + UI_GAP

            self._ui_button_row(l + UI_PAD, y - btnH, y, mx, my, self.dbgHits, [
                ("< Prev", lambda: self._dbg_select(-1)),
                ("Next >", lambda: self._dbg_select(+1)),
                ("<< Cat", lambda: self._dbg_select_category(-1)),
                ("Cat >>", lambda: self._dbg_select_category(+1)),
            ])
            y -= btnH + UI_GAP

            # The identification row — the reason this window exists.
            self._ui_button_row(l + UI_PAD, y - btnH, y, mx, my, self.dbgHits, [
                ("Isolate", lambda: setattr(self, "dbgIsolate", not self.dbgIsolate),
                 self.dbgIsolate),
                ("Blink", lambda: setattr(self, "dbgBlink", not self.dbgBlink),
                 self.dbgBlink),
                ("Mark", lambda: setattr(self, "dbgMark", not self.dbgMark),
                 self.dbgMark),
            ])
            y -= btnH + UI_GAP

            # Step size, one button per decade. The lit button is literally what a
            # "+" click adds, so the amount is never something to remember.
            _draw_text(COL_DIM, l + UI_PAD, y - btnH + 4, "Step")
            self._ui_button_row(
                l + UI_PAD + 52, y - btnH, y, mx, my, self.dbgHits,
                [(_step_label(s), (lambda v=s: setattr(self, "dbgStep", v)),
                  abs(self.dbgStep - s) < 1e-9) for s in DebugSteps],
                minWidth=52)
            y -= btnH + UI_GAP

            # SLOT PROBE. Sends the baseline with ONE slot set, so whatever the sim does
            # is caused by that slot alone. This measures the array layout instead of
            # trusting it — see the DebugProbeBaseline comment.
            _draw_text(COL_ACTIVE if self.dbgProbe is not None else COL_DIM,
                       l + UI_PAD, y - btnH + 4, "Probe")
            self._ui_button_row(
                l + UI_PAD + 52, y - btnH, y, mx, my, self.dbgHits,
                [("Off", lambda: self._dbg_set_probe(None), self.dbgProbe is None)]
                + [(str(k), (lambda s=k: self._dbg_set_probe(s)), self.dbgProbe == k)
                   for k in range(DebugParamCount)],
                minWidth=30)
            y -= btnH + 4
            if self.dbgProbe is not None:
                # What is being sent and what it SHOULD do, on screen together: the
                # answer is then a comparison, not something to remember.
                _draw_text(COL_ACTIVE, l + UI_PAD, y - btnH + 4,
                           "slot %d = %s -> expect: %s"
                           % (self.dbgProbe, self._fmt_num(DebugProbeValue),
                              DebugProbeExpect[self.dbgProbe]))
            else:
                _draw_text(COL_DIM, l + UI_PAD, y - btnH + 4,
                           "(probe sends one slot at a time to measure the layout)")
            y -= btnH + UI_GAP

            # AIM PRESETS. One click per cardinal direction, from a known state — the
            # only unambiguous way to ask "which way does the sim think this points".
            _draw_text(COL_DIM, l + UI_PAD, y - btnH + 4, "Aim")
            self._ui_button_row(
                l + UI_PAD + 52, y - btnH, y, mx, my, self.dbgHits,
                [(lbl, (lambda p=pitch, ya=yaw, s=lbl:
                        self._dbg_aim_preset(p, ya, s)))
                 for lbl, pitch, yaw in DebugAimPresets],
                minWidth=48)
            y -= btnH + UI_GAP

            # POSITION MARKERS: billboard dots on the light's origin and along its aim.
            # A spill light is invisible in air and the bright part of its pool is not
            # where it sits, so this is the only direct read on placement.
            _draw_text(COL_DIM, l + UI_PAD, y - btnH + 4, "Marker")
            x = self._ui_button_row(
                l + UI_PAD + 52, y - btnH, y, mx, my, self.dbgHits, [
                    ("Off", lambda: self._dbg_set_marker("off"),
                     self.dbgMarkerMode == "off"),
                    ("This", lambda: self._dbg_set_marker("this"),
                     self.dbgMarkerMode == "this"),
                    ("All", lambda: self._dbg_set_marker("all"),
                     self.dbgMarkerMode == "all"),
                ], minWidth=52)
            # Size lives here rather than in a row of its own: a marker too small to
            # see and a marker that is not drawn look exactly alike, so the fix has to
            # be next to the switch that turned it on.
            _draw_text(COL_DIM, x + UI_GAP, y - btnH + 4,
                       "size %s" % self._fmt_num(self.dbgMarkerSize))
            x2 = self._ui_button_row(
                x + UI_GAP + 66, y - btnH, y, mx, my, self.dbgHits,
                [("-", lambda: self._dbg_nudge_marker("size", -1)),
                 ("+", lambda: self._dbg_nudge_marker("size", +1))], minWidth=30)
            # Reach is how far the arrow and the cone ring are drawn. Adjustable
            # because the right length depends entirely on the light: a 20 cm arrow
            # was unreadable next to a pool of light a metre across.
            _draw_text(COL_DIM, x2 + UI_GAP, y - btnH + 4,
                       "reach %s" % self._fmt_num(self.dbgMarkerReach))
            self._ui_button_row(
                x2 + UI_GAP + 76, y - btnH, y, mx, my, self.dbgHits,
                [("-", lambda: self._dbg_nudge_marker("reach", -1)),
                 ("+", lambda: self._dbg_nudge_marker("reach", +1))], minWidth=30)
            y -= btnH + UI_GAP * 2

            if light:
                vals = self.dbgVals[self.dbgSel]
                base = self.dbgBase[self.dbgSel]
                for label, slot, kind in self._dbg_slots():
                    _draw_text(COL_DIM, l + UI_PAD, y - btnH + 4, label)
                    changed = abs(vals[slot] - base[slot]) > 1e-9
                    # Spread is STORED as a cosine and SHOWN as an angle. Showing the
                    # stored number would put the backwards-running value straight back
                    # on screen, which is the thing these rows exist to get rid of.
                    shown = (_cone_to_spread(vals[slot]) if kind == "spread"
                             else vals[slot])
                    _draw_text(COL_ACTIVE if changed else COL_BRIGHT,
                               l + UI_PAD + 52, y - btnH + 4, self._fmt_num(shown))
                    self._ui_button_row(
                        l + UI_PAD + 120, y - btnH, y, mx, my, self.dbgHits,
                        [("-", (lambda s=slot, k=kind: self._dbg_nudge(s, k, -1))),
                         ("+", (lambda s=slot, k=kind: self._dbg_nudge(s, k, +1)))],
                        minWidth=34)
                    if kind == "spread":
                        _draw_text(COL_DIM, l + UI_PAD + 210, y - btnH + 4,
                                   "deg half-angle   (cone: %s)"
                                   % self._fmt_num(vals[slot]))
                    elif kind in ("pitch", "yaw"):
                        # Named so the sign convention never has to be recalled: it is
                        # the one thing about an aim that cannot be checked by eye
                        # without already knowing which way you asked it to point.
                        _draw_text(COL_DIM, l + UI_PAD + 210, y - btnH + 4,
                                   "deg  %s" % (_pitch_word(vals[slot])
                                                if kind == "pitch"
                                                else _yaw_word(vals[slot])))
                    elif kind == "pos":
                        # Offsets; show the absolute coordinate they produce, which is
                        # the number that goes into the .phdsl.
                        axis = slot - DebugPosSlot
                        _draw_text(COL_DIM, l + UI_PAD + 210, y - btnH + 4,
                                   "-> %s" % self._fmt_num(
                                       self._dbg_abs_pos(self.dbgSel)[axis]))
                    y -= btnH + 4
                y -= UI_GAP

            # A/B row, labelled with what is SHOWING rather than what clicking does:
            # while "Original" is up the tuner controls nothing, which has to be obvious.
            self._ui_button_row(l + UI_PAD, y - btnH, y, mx, my, self.dbgHits, [
                ("Debug lights", self._dbg_toggle_compare, bool(self.dbgCompare)),
                ("Original lines", self._dbg_toggle_compare, not self.dbgCompare),
            ])
            # `Norm dir` used to live here. It is gone because it cannot apply any
            # more: aim is edited as angles and the vector is derived from them, so
            # every value this window emits is already unit length.
            y -= btnH + UI_GAP

            # ORIENTATION SEARCH, applied to every light at once; the readout below the
            # buttons is the answer to report. The POSITION half needs the wrappers —
            # without them `permute(pos) - pos` has nothing to drive — while aim is
            # permutable either way, going through the light's own dataref.
            orientRow = []
            if self.dbgPosTunable:
                orientRow += [
                    ("Orient <", lambda: self._dbg_cycle_orient("pos", -1)),
                    ("Orient >", lambda: self._dbg_cycle_orient("pos", +1)),
                ]
            orientRow += [
                ("Dir <", lambda: self._dbg_cycle_orient("dir", -1)),
                ("Dir >", lambda: self._dbg_cycle_orient("dir", +1)),
            ]
            if self.dbgPosTunable:
                orientRow.append(("Link", lambda: setattr(self, "dbgOrientLink",
                                                          not self.dbgOrientLink),
                                  self.dbgOrientLink))
            orientRow.append(("Reset", self._dbg_reset_orient))
            self._ui_button_row(l + UI_PAD, y - btnH, y, mx, my, self.dbgHits,
                                orientRow, minWidth=52)
            y -= btnH + 4
            if self.dbgPosTunable:
                moved = self.dbgOrientPos or self.dbgOrientDir
                # Says whose side the markers are on, because that is the whole method:
                # cycle `Dir` until the light agrees with the arrow.
                text = "pos %s     dir %s     (markers show the ASKED aim)" % (
                    self._dbg_orient_text("pos"), self._dbg_orient_text("dir"))
            else:
                moved = self.dbgOrientDir
                text = "dir %s        (position tuning off — this OBJ was built " \
                    "--no-debug-pos)" % self._dbg_orient_text("dir")
            _draw_text(COL_ACTIVE if moved else COL_DIM, l + UI_PAD, y - btnH + 4, text)
            y -= btnH + UI_GAP

            self._ui_button_row(l + UI_PAD, y - btnH, y, mx, my, self.dbgHits, [
                ("Log DSL", self._dbg_log_dsl),
                ("Revert", self._dbg_revert),
                ("Revert All", lambda: self._dbg_revert(True)),
                ("Rescan", self._dbg_rescan),
            ])
        except Exception:
            _log("DbgDraw error:\n" + traceback.format_exc())

    def DbgClick(self, windowID, x, y, inMouse, refCon):
        return self._ui_click(self.dbgHits, x, y, inMouse)

    def ShowDebugWindow(self):
        if not self.dbgLights:
            self.dbgLoadTried = True
            self._dbg_load()
        if self.dbgWin is not None:
            xp.setWindowIsVisible(self.dbgWin, 1)
            xp.bringWindowToFront(self.dbgWin)
            return
        fh = _font_height()
        height = (UI_TOP_INSET + 2 * (fh + 8) + UI_GAP
                  # nav, identify, step, probe (+ its readout), aim-preset, marker
                  + 7 * (fh + 10) + UI_GAP * 7 + 4
                  + len(self._dbg_slots()) * (fh + 14)
                  # A/B row, orientation buttons, orientation readout, actions row
                  + 4 * (fh + 10) + UI_GAP * 3 + UI_PAD * 3)
        # Remember what the height was sized for: swapping between a --debug-pos build
        # and a plain one changes the row count.
        self.dbgWinSlots = len(self._dbg_slots())
        # 560 wide, not 460: the marker row carries Off/This/All plus a size and a
        # reach nudge, and at 460 the reach buttons fell off the right edge — where
        # they are unclickable but still drawn by the hit list, i.e. invisibly broken.
        self.dbgWin = self._create_window("Photon Dev - Live Light Debug",
                                          560, height, self.DbgDraw, self.DbgClick)

    # ---------------------------------------------------------------- menu
    def CreateMenu(self):
        try:
            self.pluginsMenuItem = xp.appendMenuItem(xp.findPluginsMenu(), "Photon Dev", 0)
            self.menuID = xp.createMenu("Photon Dev", xp.findPluginsMenu(),
                                        self.pluginsMenuItem, self.MenuHandler, 0)
            # FLAT menu, no submenus: XPPython3 4.7a1 crashes in native menu
            # click-dispatch at level 2+ (see docs/dev-tools.md). The native .xpl is
            # free to nest; this plugin is not.
            xp.appendMenuItem(self.menuID, "Quick Reload (snapshot -> reload -> restore)", "reload")
            xp.appendMenuItem(self.menuID, "Plain Reload (reload only)", "plain")
            xp.appendMenuItem(self.menuID, "Release Camera", "release")
            if hasattr(xp, "appendMenuSeparator"):
                xp.appendMenuSeparator(self.menuID)
            xp.appendMenuItem(self.menuID, "Cinematic Camera...", "cine")
            xp.appendMenuItem(self.menuID, "Cockpit Light Tuner...", "tuner")
            xp.appendMenuItem(self.menuID, "Live Light Debug...", "lightdebug")
            if hasattr(xp, "appendMenuSeparator"):
                xp.appendMenuSeparator(self.menuID)
            xp.appendMenuItem(self.menuID, "Log Plugin List", "plugins")
            _log("menu built")
        except Exception:
            _log("CreateMenu error:\n" + traceback.format_exc())

    def DestroyMenu(self):
        try:
            if self.menuID is not None:
                xp.destroyMenu(self.menuID)
                self.menuID = None
            if self.pluginsMenuItem is not None:
                try:
                    xp.removeMenuItem(xp.findPluginsMenu(), self.pluginsMenuItem)
                except Exception:
                    pass
                self.pluginsMenuItem = None
        except Exception:
            _log("DestroyMenu error:\n" + traceback.format_exc())

    def MenuHandler(self, menuRefCon, itemRefCon):
        try:
            if itemRefCon == "reload":
                self.Trigger()
            elif itemRefCon == "plain":
                self.PlainReload()
            elif itemRefCon == "release":
                self._release_camera_to_free()
            elif itemRefCon == "cine":
                self.ShowCineWindow()
            elif itemRefCon == "tuner":
                self.ShowTunerWindow()
            elif itemRefCon == "lightdebug":
                self.ShowDebugWindow()
            elif itemRefCon == "plugins":
                self._log_plugin_list()
        except Exception:
            _log("MenuHandler error:\n" + traceback.format_exc())

    def _log_plugin_list(self):
        aircraftDir = self._current_aircraft_dir()
        _log("---- loaded plugins (name | will-disable? | path) ----")
        _log("aircraft folder: %s" % aircraftDir)
        for pid, name, sig, path in self._iter_plugins():
            if self._should_disable(name, path, aircraftDir):
                will = "DISABLE"
            elif self._is_aircraft_plugin(path, aircraftDir) and self._skip(name):
                will = "skip(excl)"
            else:
                will = ""
            _log("  %-32s | %-10s | %s" % (name, will, path))
        _log("------------------------------------------------------")
