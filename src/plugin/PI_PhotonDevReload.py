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

COL_TEXT   = (0.86, 0.86, 0.86)
COL_BRIGHT = (1.0, 1.0, 1.0)
COL_DIM    = (0.55, 0.55, 0.58)
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
        except Exception:
            _log("XPluginStop error:\n" + traceback.format_exc())

    def XPluginReceiveMessage(self, inFromWho, inMessage, inParam):
        try:
            if inMessage != MsgPlaneLoaded or inParam != 0:
                return
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
                # Mounted fixtures (the map lamps) nest several ANIM_begin levels for
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
                # geometry and differ only in colour, so any one of them is right.
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
                # colour variants into one.
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
