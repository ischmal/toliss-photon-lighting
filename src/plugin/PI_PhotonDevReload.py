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
     3. Disable the ToLiss aircraft plugins (matched by NAME: "AirbusFBW", "SASL") - the
        same crash-avoidance step you do by hand in Plugin Admin before reloading an OBJ.
     4. Reload the aircraft (sim/operation/reload_aircraft) - re-reads the edited OBJ.
     5. When the reloaded plane reports loaded (MSG_PLANE_LOADED), SETTLE: re-enable the
        plugins, then re-assert the snapshotted datarefs every tick for a few seconds so
        ToLiss's own cold-start init can't clobber them (power back on, your light switches
        back on).
     6. Restore the CAMERA to the exact pose you were looking from, then hand control back.

WHY A COMMAND (not just a dataref/menu): commands are bindable to keys and joystick
buttons, so you can fire the whole cycle without moving the mouse to a menu while framing
a shot outside. A "Photon Dev" Plugins menu is also provided (Quick Reload / Release
Camera / Log Plugin List) as a mouse fallback.

TUNE IN-SIM: the timing constants and (especially) the external-power dataref name below
are the parts to confirm in the sim. Every snapshot dataref is logged with found/value at
trigger time, so tailing XPPython3Log.txt tells you immediately whether a name resolves.
"""
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

# Command + menu identity
CommandName = "ToLissPhoton/dev/quick_reload"
CommandDesc = "Photon Dev: snapshot -> reload aircraft -> restore state & camera"
ReleaseCameraCommandName = "ToLissPhoton/dev/release_camera"
ReleaseCameraCommandDesc = "Photon Dev: release the held camera back to free-look control"
# ====================================================================================


# XPLM constants with integer fallbacks (names occasionally differ by version)
CommandBegin   = getattr(xp, "CommandBegin", 0)
MsgPlaneLoaded = getattr(xp, "MSG_PLANE_LOADED", 102)
PhaseAfterFM   = getattr(xp, "FlightLoop_Phase_AfterFlightModel", 1)
CamForever     = getattr(xp, "ControlCameraForever", 2)
WinDecoNone    = getattr(xp, "WindowDecorationNone", 0)
WinLayerOverlay = getattr(xp, "WindowLayerFlightOverlay", getattr(xp, "WindowLayerFloatingWindows", 2))

TYPE_INT      = getattr(xp, "Type_Int", 1)
TYPE_FLOAT    = getattr(xp, "Type_Float", 2)
TYPE_DOUBLE   = getattr(xp, "Type_Double", 4)
TYPE_FLOATARR = getattr(xp, "Type_FloatArray", 8)
TYPE_INTARR   = getattr(xp, "Type_IntArray", 16)

# state machine
ST_IDLE, ST_AWAIT_LOAD, ST_SETTLING, ST_CAMERA = 0, 1, 2, 3


def _log(msg):
    try:
        xp.log("Photon Dev: " + msg)
    except Exception:
        pass


class PythonInterface:
    def __init__(self):
        self.Name = "Photon Dev Reload"
        self.Sig  = "toliss.photon.devreload"
        self.Desc = "Dev tool: one-key reload + state/camera restore for light-position iteration."

        self.state = ST_IDLE
        self.cmdRef = None
        self.releaseCmdRef = None
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

    # ---------------------------------------------------------------- lifecycle
    def XPluginStart(self):
        try:
            self.cmdRef = xp.createCommand(CommandName, CommandDesc)
            xp.registerCommandHandler(self.cmdRef, self.QuickReloadCmd, 1, 0)
            self.releaseCmdRef = xp.createCommand(ReleaseCameraCommandName, ReleaseCameraCommandDesc)
            xp.registerCommandHandler(self.releaseCmdRef, self.ReleaseCameraCmd, 1, 0)
            _log("started; command '%s' registered (bind it to a key/button)" % CommandName)
        except Exception:
            _log("XPluginStart error:\n" + traceback.format_exc())
        return self.Name, self.Sig, self.Desc

    def XPluginEnable(self):
        try:
            self.driverLoopID = xp.createFlightLoop(self.Driver, PhaseAfterFM, 0)
            # Separate watchdog loop for the camera hold: it must keep ticking to
            # re-grab the camera after the settle state machine has gone idle.
            self.camWatchLoopID = xp.createFlightLoop(self.CameraWatchLoop, PhaseAfterFM, 0)
            self.CreateMenu()
        except Exception:
            _log("XPluginEnable error:\n" + traceback.format_exc())
        return 1

    def XPluginDisable(self):
        try:
            self._release_camera()
            if self.driverLoopID is not None:
                xp.destroyFlightLoop(self.driverLoopID)
                self.driverLoopID = None
            if self.camWatchLoopID is not None:
                xp.destroyFlightLoop(self.camWatchLoopID)
                self.camWatchLoopID = None
            self.DestroyMenu()
        except Exception:
            _log("XPluginDisable error:\n" + traceback.format_exc())

    def XPluginStop(self):
        try:
            if self.cmdRef is not None:
                xp.unregisterCommandHandler(self.cmdRef, self.QuickReloadCmd, 1, 0)
            if self.releaseCmdRef is not None:
                xp.unregisterCommandHandler(self.releaseCmdRef, self.ReleaseCameraCmd, 1, 0)
        except Exception:
            _log("XPluginStop error:\n" + traceback.format_exc())

    def XPluginReceiveMessage(self, inFromWho, inMessage, inParam):
        try:
            if inMessage == MsgPlaneLoaded and inParam == 0 and self.state == ST_AWAIT_LOAD:
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

    def ReleaseCameraCmd(self, inCommand, inPhase, inRefcon):
        if inPhase == CommandBegin:
            self._release_camera_to_free()
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
            self._release_camera()
            self._snapshot_datarefs()
            self._disable_plugins()
            self._issue_reload()
        except Exception:
            _log("Trigger error:\n" + traceback.format_exc())
            self.state = ST_IDLE

    def _issue_reload(self):
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
        # Set AWAIT_LOAD *before* commandOnce: the reload (and MSG_PLANE_LOADED) can fire
        # synchronously inside this call, so ReceiveMessage must already see AWAIT_LOAD.
        self.state = ST_AWAIT_LOAD
        _log("issuing %s (handler context)" % ReloadCommand)
        xp.commandOnce(cmd)
        _log("reload issued; awaiting MSG_PLANE_LOADED")

    def Driver(self, sinceLast, sinceLoop, counter, refCon):
        try:
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

    # ---------------------------------------------------------------- menu
    def CreateMenu(self):
        try:
            self.pluginsMenuItem = xp.appendMenuItem(xp.findPluginsMenu(), "Photon Dev", 0)
            self.menuID = xp.createMenu("Photon Dev", xp.findPluginsMenu(),
                                        self.pluginsMenuItem, self.MenuHandler, 0)
            xp.appendMenuItem(self.menuID, "Quick Reload (snapshot -> reload -> restore)", "reload")
            xp.appendMenuItem(self.menuID, "Release Camera", "release")
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
            elif itemRefCon == "release":
                self._release_camera_to_free()
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
