"""
PI_ToLissPhoton.py  -  XPPython3 plugin for ToLiss Photon Lighting

  * Registers NINE per-category light-type datarefs at SIM STARTUP (so the aircraft
    OBJ can bind them before it loads). Each is 0 = halogen/xenon, 1 = LED:
        ToLissPhoton/exterior/{taxi,to,rwyturnoff,wing,landing,beacon,nav,strobe,logo}
    Plus a deprecated read-only alias ToLissPhoton/exterior/is_led = 1 only when ALL
    categories resolve LED, else 0 (safety net for any OBJ not yet migrated).

  * Reshapes the exterior BEACON and STROBE flash waveforms every frame, picking a
    per-light waveform from that light's resolved category value (beacon/strobe = 1 ->
    crisp square LED flash; 0 -> xenon strike + decay). The engine runs on the
    after-flight-model flight-loop phase so its write lands AFTER ToLiss's own per-frame
    write and therefore wins (gotcha #5; verified in-sim). Reading the category value
    straight from memory (self.values) means there is no dataref round-trip and none of
    the old int/float accessor race the two-plugin split needed.

  * Shows a "ToLiss Photon" menu ONLY while a ToLiss aircraft is loaded:
        Auto / Classic / Hybrid LED / Full LED    (radio)
        ----                                      (separator)
        Custom...                                 (opens a widget window with a
                                                   checkbox per category; checked = LED)

  * Resolves the chosen profile into the 9 category values, writing them directly.
    Auto infers a profile from the airframe and re-infers each second.

  * Saves/loads the selection PER LIVERY to
        <X-Plane>/Output/preferences/ToLissPhoton_profiles.json

Persistence: Auto is NEVER saved (absence == Auto). Named profile saves {"profile": n}.
Custom saves {"profile": 4, "categories": {all 9}}; changing any category => Custom.

WHY CUSTOM IS A WINDOW, NOT A SUBMENU: XPPython3 4.7a1 crashes the sim in native menu
click-dispatch for any item in a submenu-of-a-submenu (menu level 2+). The top menu is
level 1 and works; a nested "Custom >" submenu (and its category sub-submenus) put the
category items at level 2/3 and crashed on click BEFORE the Python handler ran (no
handler log; X-Plane died with "Forwarding exception..."). So Custom is a plain level-1
item that opens a widget window (verified idiom from XPPython3's own updater plugin). Do
NOT reintroduce nested submenus. Menu build is defensive - failures are logged and
degrade gracefully. Check XPPython3Log.txt if anything misbehaves.

Install: copy into Resources/plugins/PythonPlugins/ (requires XPPython3). Delete any
stale __pycache__ there and remove any older PI_PhotonMenu.py. The old FlyWithLua
script (ToLissPhoton.lua) must NOT run alongside this plugin - it would fight for the
same per-frame brightness writes - so this plugin auto-deletes it from the FlyWithLua
Scripts (and quarantine) folders at startup (see _RemoveLegacyLua). An upgrader may need
one sim restart for that to take full effect.
"""
import os
import json
import math
import traceback
from XPPython3 import xp

# ---- categories (this order is also the Custom submenu order) ----
# (key, dataref name, menu label, label for the "0" / classic option)
Categories = (
    ("taxi",       "ToLissPhoton/exterior/taxi",       "Taxi Light",      "Halogen"),
    ("to",         "ToLissPhoton/exterior/to",         "T.O. Light",      "Halogen"),
    ("rwyturnoff", "ToLissPhoton/exterior/rwyturnoff", "Runway Turn Off", "Halogen"),
    ("wing",       "ToLissPhoton/exterior/wing",       "Wing Lights",     "Halogen"),
    ("landing",    "ToLissPhoton/exterior/landing",    "Landing Lights",  "Halogen"),
    ("beacon",     "ToLissPhoton/exterior/beacon",     "Beacon",          "Xenon"),
    ("nav",        "ToLissPhoton/exterior/nav",        "Nav Lights",      "Halogen"),
    ("strobe",     "ToLissPhoton/exterior/strobe",     "Strobes",         "Xenon"),
    ("logo",       "ToLissPhoton/exterior/logo",       "Logo Lights",     "Halogen"),
)
CategoryKeys = [c[0] for c in Categories]

IsLedDataRefName   = "ToLissPhoton/exterior/is_led"   # deprecated alias
LiveryIndexDataRef = "sim/aircraft/view/acf_livery_index"
IcaoDataRef        = "sim/aircraft/view/acf_ICAO"

# Retired FlyWithLua script names to auto-delete on startup (see _RemoveLegacyLua). The
# waveform engine used to live in ToLissPhoton.lua; if an upgraded install still has it,
# both it and this plugin would drive the same per-frame brightness writes and fight.
LegacyLuaNames = ("ToLissPhoton.lua", "photon_toliss_lights.lua")

# ---- profiles ----
PROFILE_AUTO, PROFILE_CLASSIC, PROFILE_HYBRID, PROFILE_FULL_LED, PROFILE_CUSTOM = 0, 1, 2, 3, 4

# (item refCon, label, profile value) - menu order; the separator goes before Custom
ProfileItems = (
    ("ToLissPhoton_auto",    "Auto",       PROFILE_AUTO),
    ("ToLissPhoton_classic", "Classic",    PROFILE_CLASSIC),
    ("ToLissPhoton_hybrid",  "Hybrid LED", PROFILE_HYBRID),
    ("ToLissPhoton_full",    "Full LED",   PROFILE_FULL_LED),
    ("ToLissPhoton_custom",  "Custom...",  PROFILE_CUSTOM),
)

# Categories that are LED in the Hybrid LED profile (position/anti-collision lights);
# the beam lights (taxi, to, rwyturnoff, wing, landing) stay halogen. Edit to taste.
HybridLedCategories = ("beacon", "strobe", "nav")

# ---- Auto gating (configurable; fill in MFRL once the dataref is known) ----
# anim/SHARK is the ToLiss geometry flag used by the OBJ: 0 = wingtip fence, 1 = sharklet.
SharkletGate      = "anim/SHARK"
SharkletThreshold = 0.5
MfrlGate          = "AirbusFBW/LandingLightLocation"      # when set AND present above threshold => Full LED (top priority)
MfrlThreshold     = 0.5
AutoFallbackProfile = PROFILE_CLASSIC   # used when no gate resolves


# ============================== WAVEFORM ENGINE ======================================
# Ported from ToLissPhoton.lua (retired). Reshapes the beacon/strobe flash each frame.
#
# Each light is a Channel with a cycle Period, an Offset, and a list of Pulses:
#     {"At", "Duration", "Shape", "Hold", "Tau", "Peak"}
#   Shape = "Square" | "Decay" | "Triangle" | "Sine" | "RampUp" | "RampDown"
#   Hold  = fraction of the pulse held at full peak before the shape runs (0..1)
#   Tau   = decay sharpness for Shape="Decay" (smaller = sharper)
#   Peak  = 0..1 peak brightness (default 1)
# Advancing uses a carry-preserving timer (no drift); rendering uses a RenderAge that
# resets to 0 at each onset, so the onset frame is always the exact peak and low frame
# rates collapse to one peak frame then zero (no half-finished fade).
#
# WaveformProfiles[0] = LED (square), [1] = Classic (halogen/xenon decay). The beacon
# and strobe halves are chosen independently from the resolved beacon/strobe category
# values. Strobe indices are guesses until confirmed in-sim (0,1 = double-flash wingtips,
# 2,3 = single tail flash).
WaveformProfiles = {
    0: {  # LED
        "Beacon": {"Period": 1.0, "Offset": 0.5,
                   "Pulses": [{"At": 0.00, "Duration": 0.1, "Shape": "Square"}]},
        "Strobe": {"Period": 1.0, "Offset": 0.0, "Index": {
            0: [{"At": 0.00, "Duration": 0.05, "Shape": "Square"},
                {"At": 0.15, "Duration": 0.05, "Shape": "Square"}],   # double
            1: [{"At": 0.00, "Duration": 0.05, "Shape": "Square"},
                {"At": 0.15, "Duration": 0.05, "Shape": "Square"}],   # double
            2: [{"At": 0.00, "Duration": 0.1,  "Shape": "Square"}],   # single
            3: [{"At": 0.00, "Duration": 0.08, "Shape": "Square"}],   # single
        }},
    },
    1: {  # Classic
        "Beacon": {"Period": 1.0, "Offset": 0.5,
                   "Pulses": [{"At": 0.00, "Duration": 0.06, "Hold": 0.40,
                               "Shape": "Decay", "Tau": 0.30}]},
        "Strobe": {"Period": 1.0, "Offset": 0.0, "Index": {
            0: [{"At": 0.00, "Duration": 0.04, "Shape": "Square"},
                {"At": 0.12, "Duration": 0.04, "Shape": "Square"}],
            1: [{"At": 0.00, "Duration": 0.04, "Shape": "Square"},
                {"At": 0.12, "Duration": 0.04, "Shape": "Square"}],
            2: [{"At": 0.00, "Duration": 0.06, "Hold": 0.40, "Shape": "Decay", "Tau": 0.30}],
            3: [{"At": 0.00, "Duration": 0.04, "Shape": "Square"}],
        }},
    },
}

# On/off gating + timing (mirrors the Lua constants).
GateThreshold         = 0.5     # a dataref gate counts as "on" above this
InvertStrobeGate      = False   # set True if strobes come out inverted
ActivityThreshold     = 0.05    # ToLiss value above this = "ToLiss is lighting it"
ActivityWindowSeconds = 1.2     # hold "active" this long after last seen lit (auto gate)
MaxDeltaTime          = 0.25    # clamp frame dt (smooths over pauses)

# sim datarefs the engine reads/writes.
BeaconRatioDataRef = "sim/flightmodel2/lights/beacon_brightness_ratio"
StrobeRatioDataRef = "sim/flightmodel2/lights/strobe_brightness_ratio"
OverrideDataRef    = "sim/flightmodel2/lights/override_beacons_and_strobes"
SimTimeDataRef     = "sim/time/total_running_time_sec"
StrobeGateDataRef  = "sim/cockpit/electrical/strobe_lights_on"   # zero-lag strobe gate


def _ClampBrightness(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def _Envelope(shape, progress, tau):
    if shape == "Square":
        return 1.0
    if shape == "Decay":
        # normalised exponential: exactly 1.0 at progress 0, exactly 0.0 at progress 1,
        # for any Tau. Smaller Tau = sharper initial drop.
        t = tau or 0.30
        tail = math.exp(-1.0 / t)
        return (math.exp(-progress / t) - tail) / (1.0 - tail)
    if shape == "RampUp":
        return progress
    if shape == "RampDown":
        return 1.0 - progress
    if shape == "Triangle":
        return 1.0 - abs(2.0 * progress - 1.0)
    if shape == "Sine":
        return math.sin(progress * math.pi)
    return 1.0


def _BuildSegments(pulses, period):
    ordered = sorted(pulses, key=lambda p: p["At"])
    segments = []
    cursor = 0.0
    for pulse in ordered:
        at = pulse["At"]
        if at > cursor + 1e-6:
            segments.append({"Duration": at - cursor, "IsOn": False})
        segments.append({
            "Duration": pulse["Duration"], "IsOn": True,
            "Shape": pulse.get("Shape", "Square"), "Tau": pulse.get("Tau", 0.30),
            "Hold": pulse.get("Hold", 0.0), "Peak": pulse.get("Peak", 1.0),
        })
        cursor = max(cursor, at) + pulse["Duration"]
    if cursor < period - 1e-6:
        segments.append({"Duration": period - cursor, "IsOn": False})
    elif cursor > period + 1e-3:
        _log("WARNING pulses total %.3fs exceed period %.3fs" % (cursor, period))
    return segments


class _Channel:
    """One light's tiled on/off segment loop. SegmentIndex is 0-based (Python)."""
    __slots__ = ("Segments", "Period", "Offset", "TotalDuration",
                 "SegmentIndex", "SegmentElapsed", "RenderAge")

    def __init__(self, pulses, period, offset):
        self.Segments = _BuildSegments(pulses, period)
        self.Period = period
        self.Offset = offset
        self.TotalDuration = sum(s["Duration"] for s in self.Segments)
        self.SegmentIndex = 0
        self.SegmentElapsed = 0.0
        self.RenderAge = 0.0

    def Seek(self, targetTime):
        if self.TotalDuration <= 0:
            self.SegmentIndex = 0
            self.SegmentElapsed = 0.0
            self.RenderAge = 0.0
            return
        targetTime = targetTime % self.TotalDuration
        if targetTime < 0:
            targetTime += self.TotalDuration
        accumulated = 0.0
        for index, segment in enumerate(self.Segments):
            if targetTime < accumulated + segment["Duration"]:
                self.SegmentIndex = index
                self.SegmentElapsed = targetTime - accumulated
                self.RenderAge = self.SegmentElapsed
                return
            accumulated += segment["Duration"]
        self.SegmentIndex = len(self.Segments) - 1
        self.SegmentElapsed = 0.0
        self.RenderAge = 0.0

    def Tick(self, deltaTime):
        # Render the current segment from RenderAge (onset = peak, low-FPS = peak-then-
        # zero), then advance using the carry-preserving SegmentElapsed (no drift).
        segment = self.Segments[self.SegmentIndex]
        brightness = 0.0
        if segment["IsOn"] and segment["Duration"] > 0 and self.RenderAge < segment["Duration"]:
            progress = self.RenderAge / segment["Duration"]
            if progress < 0.0:
                progress = 0.0
            elif progress > 1.0:
                progress = 1.0
            hold = segment.get("Hold", 0.0)
            if hold >= 1.0:
                progress = 0.0
            elif progress > hold:
                progress = (progress - hold) / (1.0 - hold)
            else:
                progress = 0.0
            brightness = _Envelope(segment["Shape"], progress, segment.get("Tau", 0.30)) \
                * segment.get("Peak", 1.0)

        self.SegmentElapsed += deltaTime
        self.RenderAge += deltaTime
        if self.SegmentElapsed >= segment["Duration"]:
            self.SegmentElapsed -= segment["Duration"]
            self.SegmentIndex += 1
            if self.SegmentIndex >= len(self.Segments):
                self.SegmentIndex = 0
            self.RenderAge = 0.0
        return brightness
# =====================================================================================


def CategoriesForNamedProfile(profile):
    """Return {key: 0/1} for a NAMED profile (Classic / Hybrid LED / Full LED)."""
    if profile == PROFILE_FULL_LED:
        return {k: 1 for k in CategoryKeys}
    if profile == PROFILE_HYBRID:
        return {k: (1 if k in HybridLedCategories else 0) for k in CategoryKeys}
    return {k: 0 for k in CategoryKeys}   # Classic (and any unexpected value)


# constants with integer fallbacks in case names differ by version
MenuChecked     = getattr(xp, "Menu_Checked", 2)
MenuUnchecked   = getattr(xp, "Menu_Unchecked", 1)
MsgPlaneLoaded  = getattr(xp, "MSG_PLANE_LOADED", 102)
MsgLiveryLoaded = getattr(xp, "MSG_LIVERY_LOADED", 108)


def _log(msg):
    try:
        xp.log("ToLiss Photon: " + msg)
    except Exception:
        pass


class PythonInterface:
    def __init__(self):
        self.Name = "ToLiss Photon"
        self.Sig  = "toliss.photon.lighting.menu"
        self.Desc = "ToLiss Photon Lighting: per-category profiles, datarefs, and per-livery save."

        self.profile = PROFILE_AUTO
        self.values  = {k: 0 for k in CategoryKeys}   # backing ints for the 9 datarefs
        self.accessors = {}                            # key -> accessor handle
        self.isLedAccessor = None

        self.profiles = {}          # liveryKey -> {"profile": n[, "categories": {...}]}
        self.liveryIndexRef = None

        # menu state
        self.menuCreated = False
        self.pluginsMenuItem = None
        self.menuID = None                 # top-level ToLiss Photon menu
        self.profileItemIndex = {}         # profile refCon -> item index (top menu)

        # Custom is a WIDGET WINDOW, not a submenu. XPPython3 4.7a1 crashes the sim on
        # click-dispatch for any item in a submenu-of-a-submenu (level 2+); the top menu
        # is level 1 and safe, so "Custom" is a level-1 item that opens this window.
        self.window = None                 # Custom config window (WidgetClass_MainWindow)
        self.categoryCheckbox = {}         # key -> checkbox widget id

        self.flightLoopRegistered = False

        # ---- waveform engine state (ported from ToLissPhoton.lua) ----
        self.engineLoopID   = None          # after-flight-model per-frame loop
        self.simTimeRef     = None
        self.beaconRatioRef = None
        self.strobeRatioRef = None
        self.overrideRef    = None
        self.strobeGateRef  = None
        self.lastSimTime    = -1.0
        self.overrideWarned = False
        self.beaconChannel  = None          # _Channel
        self.strobeChannels = {}            # index -> _Channel
        self.currentBeaconLed = -1          # -1 = "not built yet"
        self.currentStrobeLed = -1
        self.lastActiveBeacon = {}          # index -> last simTime seen lit (auto gate)

    # ---- dataref accessor callbacks ----
    def _makeReaders(self, key):
        # per-key closures so we don't depend on refCon plumbing
        return (lambda refCon: self.values[key],
                lambda refCon: float(self.values[key]))

    def _allLed(self):
        return 1 if all(v == 1 for v in self.values.values()) else 0

    def _readIsLedInt(self, refCon):   return self._allLed()
    def _readIsLedFloat(self, refCon): return float(self._allLed())

    # ---- lifecycle ----
    def XPluginStart(self):
        try:
            for key, name, label, classicLabel in Categories:
                readInt, readFloat = self._makeReaders(key)
                self.accessors[key] = xp.registerDataAccessor(
                    name, readInt=readInt, readFloat=readFloat)
            self.isLedAccessor = xp.registerDataAccessor(
                IsLedDataRefName, readInt=self._readIsLedInt, readFloat=self._readIsLedFloat)
            self.LoadProfilesFile()
            self._RemoveLegacyLua()
            _log("started; registered %d category datarefs" % len(self.accessors))
        except Exception:
            _log("XPluginStart error:\n" + traceback.format_exc())
        return self.Name, self.Sig, self.Desc

    def _RemoveLegacyLua(self):
        """Delete the retired FlyWithLua script(s) so an upgraded install doesn't run the
        old Lua engine alongside this plugin (both would write the beacon/strobe ratios
        every frame and fight). Checks the Scripts folder and FlyWithLua's quarantine
        folder. Best-effort: missing folders and permission errors are just logged.

        NOTE: if FlyWithLua already loaded the old script THIS session, deleting the file
        now doesn't unload the running copy - it stops loading on the next FlyWithLua
        script reload (aircraft change) or sim restart. So an upgrader may need one
        restart for the effect to be clean; after that the file is gone for good.
        """
        try:
            root = xp.getSystemPath()
        except Exception:
            root = ""
        if not root:
            return
        fwlScripts = os.path.join(root, "Resources", "plugins", "FlyWithLua")
        scriptDirs = (os.path.join(fwlScripts, "Scripts"),
                      os.path.join(fwlScripts, "Scripts (Quarantine)"))
        for directory in scriptDirs:
            for name in LegacyLuaNames:
                path = os.path.join(directory, name)
                try:
                    if os.path.isfile(path):
                        os.remove(path)
                        _log("removed legacy FlyWithLua script: %s" % path)
                except Exception:
                    _log("could NOT remove legacy script %s:\n%s"
                         % (path, traceback.format_exc()))

    def XPluginEnable(self):
        try:
            self.liveryIndexRef = xp.findDataRef(LiveryIndexDataRef)
            if not self.flightLoopRegistered:
                xp.registerFlightLoopCallback(self.FlightLoop, 1.0, 0)
                self.flightLoopRegistered = True
            self._StartEngine()
            self.UpdateMenuVisibility()
            self.LoadProfileForCurrentLivery()
        except Exception:
            _log("XPluginEnable error:\n" + traceback.format_exc())
        return 1

    def XPluginDisable(self):
        try:
            if self.flightLoopRegistered:
                xp.unregisterFlightLoopCallback(self.FlightLoop, 0)
                self.flightLoopRegistered = False
            self._StopEngine()
        except Exception:
            _log("XPluginDisable error:\n" + traceback.format_exc())

    # ---- waveform engine (ported from ToLissPhoton.lua) ----
    def _StartEngine(self):
        if self.engineLoopID is not None:
            return
        try:
            self.simTimeRef     = xp.findDataRef(SimTimeDataRef)
            self.beaconRatioRef = xp.findDataRef(BeaconRatioDataRef)
            self.strobeRatioRef = xp.findDataRef(StrobeRatioDataRef)
            self.overrideRef    = xp.findDataRef(OverrideDataRef)
            self.strobeGateRef  = xp.findDataRef(StrobeGateDataRef)
            # After-flight-model phase runs late in the frame, so our write lands after
            # ToLiss's own per-frame write and wins (verified by the write-race probe).
            phase = getattr(xp, "FlightLoop_Phase_AfterFlightModel", 1)
            self.engineLoopID = xp.createFlightLoop(self.EngineLoop, phase, 0)
            xp.scheduleFlightLoop(self.engineLoopID, -1.0, 1)   # -1 = every frame
            _log("waveform engine started (after-flight-model, every frame)")
        except Exception:
            _log("engine start error:\n" + traceback.format_exc())

    def _StopEngine(self):
        try:
            if self.engineLoopID is not None:
                xp.destroyFlightLoop(self.engineLoopID)
                self.engineLoopID = None
        except Exception:
            _log("engine stop error:\n" + traceback.format_exc())

    def _SimTime(self):
        if self.simTimeRef is not None:
            try:
                return xp.getDataf(self.simTimeRef)
            except Exception:
                return 0.0
        return 0.0

    def _ReadRatios(self, ref):
        # XPPython3's getDatavf fills a list in place and returns the count (not a list),
        # so pass an empty list and read it back.
        values = []
        xp.getDatavf(ref, values, 0, 4)
        return values

    def _RebuildBeacon(self, led, simTime):
        self.currentBeaconLed = led
        spec = WaveformProfiles[0 if led >= 1 else 1]["Beacon"]
        channel = _Channel(spec["Pulses"], spec["Period"], spec.get("Offset", 0.0))
        channel.Seek(simTime - channel.Offset)
        self.beaconChannel = channel
        _log("beacon waveform -> " + ("LED" if led >= 1 else "Classic"))

    def _RebuildStrobe(self, led, simTime):
        self.currentStrobeLed = led
        spec = WaveformProfiles[0 if led >= 1 else 1]["Strobe"]
        self.strobeChannels = {}
        for index, pulses in spec["Index"].items():
            channel = _Channel(pulses, spec["Period"], spec.get("Offset", 0.0))
            channel.Seek(simTime - channel.Offset)
            self.strobeChannels[index] = channel
        _log("strobe waveform -> " + ("LED" if led >= 1 else "Classic"))

    def _BeaconActive(self, index, sampledValue, simTime):
        # "auto" gate: infer on/off from ToLiss's own value, holding active for a window
        # so the beacon's own flash gaps (and our own off-segments) don't read as "off".
        if sampledValue > ActivityThreshold:
            self.lastActiveBeacon[index] = simTime
        return (simTime - self.lastActiveBeacon.get(index, -1e9)) < ActivityWindowSeconds

    def _StrobeGateOn(self):
        if self.strobeGateRef is None:
            return False
        try:
            on = xp.getDatai(self.strobeGateRef) > GateThreshold
        except Exception:
            try:
                on = xp.getDataf(self.strobeGateRef) > GateThreshold
            except Exception:
                return False
        return (not on) if InvertStrobeGate else on

    def EngineLoop(self, sinceLast, sinceLoop, counter, refCon):
        try:
            # Only drive lights on a ToLiss aircraft (menuCreated tracks that).
            if not self.menuCreated:
                self.lastSimTime = -1.0
                return -1.0

            simTime = self._SimTime()
            if self.lastSimTime < 0:
                self.lastSimTime = simTime
            deltaTime = simTime - self.lastSimTime
            self.lastSimTime = simTime
            if deltaTime < 0:
                deltaTime = 0.0
            elif deltaTime > MaxDeltaTime:
                deltaTime = MaxDeltaTime

            if not self.overrideWarned and self.overrideRef is not None:
                try:
                    if xp.getDatai(self.overrideRef) < 1:
                        _log("NOTE override_beacons_and_strobes is 0 - effect may not show.")
                        self.overrideWarned = True
                except Exception:
                    pass

            # follow each light's resolved category value, rebuilding only on a change
            beaconLed = 1 if self.values.get("beacon", 0) >= 1 else 0
            strobeLed = 1 if self.values.get("strobe", 0) >= 1 else 0
            if beaconLed != self.currentBeaconLed:
                self._RebuildBeacon(beaconLed, simTime)
            if strobeLed != self.currentStrobeLed:
                self._RebuildStrobe(strobeLed, simTime)

            # beacon: one waveform applied to all 4 indices, each auto-gated independently
            if self.beaconChannel is not None and self.beaconRatioRef is not None:
                brightness = self.beaconChannel.Tick(deltaTime)
                current = self._ReadRatios(self.beaconRatioRef)
                out = list(current)
                for i in range(len(out)):
                    if self._BeaconActive(i, current[i], simTime):
                        out[i] = _ClampBrightness(brightness)
                xp.setDatavf(self.beaconRatioRef, out, 0, 4)

            # strobe: per-index waveforms, gated on the strobe switch. Tick every channel
            # every frame (even when gated off) so phase stays correct when it turns on.
            if self.strobeChannels and self.strobeRatioRef is not None:
                current = self._ReadRatios(self.strobeRatioRef)
                out = list(current)
                gateOn = self._StrobeGateOn()
                for index, channel in self.strobeChannels.items():
                    value = channel.Tick(deltaTime)
                    if gateOn and 0 <= index < len(out):
                        out[index] = _ClampBrightness(value)
                xp.setDatavf(self.strobeRatioRef, out, 0, 4)
        except Exception:
            _log("EngineLoop error:\n" + traceback.format_exc())
        return -1.0   # keep running every frame

    def XPluginStop(self):
        try:
            self._StopEngine()
            self.DestroyMenu()
            if self.isLedAccessor is not None:
                xp.unregisterDataAccessor(self.isLedAccessor)
                self.isLedAccessor = None
            for key, handle in list(self.accessors.items()):
                xp.unregisterDataAccessor(handle)
            self.accessors = {}
        except Exception:
            _log("XPluginStop error:\n" + traceback.format_exc())

    def XPluginReceiveMessage(self, inFromWho, inMessage, inParam):
        try:
            if inMessage == MsgPlaneLoaded and inParam == 0:
                self.UpdateMenuVisibility()
                self.LoadProfileForCurrentLivery()
            elif inMessage == MsgLiveryLoaded and inParam == 0:
                self.LoadProfileForCurrentLivery()
        except Exception:
            _log("ReceiveMessage error:\n" + traceback.format_exc())

    # ---- Auto re-resolution (only while in Auto on a ToLiss aircraft) ----
    def FlightLoop(self, sinceLast, sinceLoop, counter, refCon):
        try:
            if self.menuCreated and self.profile == PROFILE_AUTO:
                before = dict(self.values)
                self.ApplyProfileToValues()
                if self.values != before:
                    self.UpdateChecks()
                    _log("Auto re-resolved -> %s" % self.values)
        except Exception:
            _log("FlightLoop error:\n" + traceback.format_exc())
        return 1.0

    # ---- ToLiss detection + menu visibility ----
    def IsToLiss(self):
        try:
            fileName, path = xp.getNthAircraftModel(0)
        except Exception:
            return False
        blob = ((path or "") + "|" + (fileName or "")).lower()
        return "toliss" in blob

    def UpdateMenuVisibility(self):
        if self.IsToLiss():
            if not self.menuCreated:
                self.CreateMenu()
        else:
            if self.menuCreated:
                self.DestroyMenu()

    # ---- profile resolution ----
    def AircraftIcao(self):
        try:
            ref = xp.findDataRef(IcaoDataRef)
            if ref is not None:
                value = xp.getDatas(ref)
                if isinstance(value, bytes):
                    value = value.decode("ascii", "ignore")
                return (value or "").strip().upper()
        except Exception:
            pass
        return ""

    def GateActive(self, path, threshold):
        """True/False if the gate dataref exists, else None (not configured/present).

        Reads according to the dataref's *published* type. A float read of an
        int-only dataref (e.g. AirbusFBW/LandingLightLocation) returns 0.0
        WITHOUT raising, so a try/except float-then-int fallback never fires
        (see CLAUDE.md gotcha #4). Query the type and pick the right accessor.
        """
        if not path:
            return None
        try:
            ref = xp.findDataRef(path)
            if ref is None:
                return None
            types = xp.getDataRefTypes(ref)
            # Type_Float = 2, Type_Double = 4, Type_Int = 1 (bitmask).
            if types & (2 | 4):
                return xp.getDataf(ref) > threshold
            if types & 1:
                return xp.getDatai(ref) > threshold
            # Unknown/unspecified type: try int then float, taking the max.
            return max(xp.getDatai(ref), xp.getDataf(ref)) > threshold
        except Exception:
            return None

    def ResolveAutoProfile(self):
        icao = self.AircraftIcao()
        if icao.startswith("A34"):
            return PROFILE_CLASSIC            # A340: always Classic
        if self.GateActive(MfrlGate, MfrlThreshold):
            return PROFILE_FULL_LED           # MFRL is the strongest positive signal
        if icao in ("A339", "A338"):
            return PROFILE_HYBRID             # A330neo: default Hybrid
        shark = self.GateActive(SharkletGate, SharkletThreshold)
        if shark is True:
            return PROFILE_HYBRID             # sharklets
        if shark is False:
            return PROFILE_CLASSIC            # wingtip fences
        return AutoFallbackProfile

    def ApplyProfileToValues(self):
        """Set self.values from self.profile. Custom keeps its explicit values."""
        if self.profile == PROFILE_CUSTOM:
            return
        effective = self.ResolveAutoProfile() if self.profile == PROFILE_AUTO else self.profile
        self.values = CategoriesForNamedProfile(effective)

    # ---- menu ----
    def CreateMenu(self):
        # Top profile menu: same known-good idiom the single-profile version used.
        # Every item lives at menu level 1 (Auto/Classic/Hybrid/Full LED/Custom); none
        # owns a submenu, so all are safe to click and safe to checkmark. Custom opens
        # a widget window (see MenuHandler) instead of a level-2 submenu.
        self.pluginsMenuItem = xp.appendMenuItem(xp.findPluginsMenu(), "ToLiss Photon", 0)
        self.menuID = xp.createMenu("ToLiss Photon", xp.findPluginsMenu(),
                                    self.pluginsMenuItem, self.MenuHandler, 0)
        self.profileItemIndex = {}
        for refCon, label, value in ProfileItems:
            if value == PROFILE_CUSTOM:
                self._TryAppendSeparator()
            self.profileItemIndex[refCon] = xp.appendMenuItem(self.menuID, label, refCon)

        self.menuCreated = True
        self.UpdateChecks()
        _log("menu built (%d profile items)" % len(self.profileItemIndex))

    def _TryAppendSeparator(self):
        try:
            if hasattr(xp, "appendMenuSeparator"):
                xp.appendMenuSeparator(self.menuID)
        except Exception:
            _log("appendMenuSeparator failed (skipping):\n" + traceback.format_exc())

    def DestroyMenu(self):
        try:
            self.DestroyCustomWindow()
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
        self.menuCreated = False

    def MenuHandler(self, menuRefCon, itemRefCon):
        # top-level profile picks; Custom opens the config window rather than selecting.
        try:
            for refCon, label, value in ProfileItems:
                if itemRefCon == refCon:
                    if value == PROFILE_CUSTOM:
                        self.ShowCustomWindow()
                    else:
                        self.SelectProfile(value)
                    break
        except Exception:
            _log("MenuHandler error:\n" + traceback.format_exc())

    # ---- Custom config window (replaces the crash-prone level-2 submenu) ----
    def ShowCustomWindow(self):
        try:
            if self.window is None:
                self._BuildCustomWindow()
            if self.window is not None:
                self.RefreshWindowCheckboxes()
                xp.showWidget(self.window)
                if hasattr(xp, "bringRootWidgetToFront"):
                    xp.bringRootWidgetToFront(self.window)
        except Exception:
            _log("ShowCustomWindow error:\n" + traceback.format_exc())

    def _BuildCustomWindow(self):
        # Verified idiom from XPPython3's own updater/preferences.py: a MainWindow with
        # WidgetClass_Button checkboxes (RadioButton type + ButtonBehaviorCheckBox).
        self.window = None
        self.categoryCheckbox = {}
        try:
            fontID = xp.Font_Proportional
            _w, strHeight, _ignore = xp.getFontDimensions(fontID)
            rowH = strHeight + 8
            header = "Checked = LED     Unchecked = classic (halogen/xenon)"
            labelW = 0
            for key, name, label, classicLabel in Categories:
                labelW = max(labelW, int(xp.measureString(fontID, label)))
            width = max(labelW + 90, int(xp.measureString(fontID, header)) + 40)
            height = rowH * (len(Categories) + 2) + 30

            # Center once at creation, then leave positioning FREE so the window can be
            # dragged and stays put. (WindowCenterOnMonitor re-centers every frame, which
            # snaps the window back to the middle whenever the user drags it.)
            left, top = 130, 560
            try:
                sLeft, sTop, sRight, sBottom = xp.getScreenBoundsGlobal()
                left = int((sLeft + sRight) / 2 - width / 2)
                top = int((sTop + sBottom) / 2 + height / 2)
            except Exception:
                pass
            right, bottom = left + width, top - height

            self.window = xp.createWidget(left, top, right, bottom, 1,
                                          "ToLiss Photon - Custom Lighting", 1, 0,
                                          xp.WidgetClass_MainWindow)
            xp.setWidgetProperty(self.window, xp.Property_MainWindowHasCloseBoxes, 1)
            try:
                winID = xp.getWidgetUnderlyingWindow(self.window)
                xp.setWindowPositioningMode(winID, xp.WindowPositionFree, -1)
            except Exception:
                pass
            xp.addWidgetCallback(self.window, self.WindowCallback)

            y = top - rowH
            xp.createWidget(left + 15, y, right - 10, y - strHeight, 1, header, 0,
                            self.window, xp.WidgetClass_Caption)
            y -= rowH + 4
            for key, name, label, classicLabel in Categories:
                cb = xp.createWidget(left + 18, y, left + 30, y - strHeight, 1, "", 0,
                                     self.window, xp.WidgetClass_Button)
                xp.setWidgetProperty(cb, xp.Property_ButtonType, xp.RadioButton)
                xp.setWidgetProperty(cb, xp.Property_ButtonBehavior, xp.ButtonBehaviorCheckBox)
                xp.createWidget(left + 42, y, left + 42 + labelW, y - strHeight, 1, label, 0,
                                self.window, xp.WidgetClass_Caption)
                self.categoryCheckbox[key] = cb
                y -= rowH
            _log("custom window built (%d checkboxes)" % len(self.categoryCheckbox))
        except Exception:
            _log("Custom window build failed:\n" + traceback.format_exc())
            self.window = None

    def RefreshWindowCheckboxes(self):
        if self.window is None:
            return
        try:
            for key, cb in self.categoryCheckbox.items():
                xp.setWidgetProperty(cb, xp.Property_ButtonState,
                                     1 if self.values.get(key) == 1 else 0)
        except Exception:
            _log("RefreshWindowCheckboxes error:\n" + traceback.format_exc())

    def DestroyCustomWindow(self):
        try:
            if self.window is not None:
                xp.destroyWidget(self.window, 1)
        except Exception:
            _log("DestroyCustomWindow error:\n" + traceback.format_exc())
        self.window = None
        self.categoryCheckbox = {}

    def WindowCallback(self, inMessage, inWidget, inParam1, inParam2):
        try:
            if inMessage == xp.Message_CloseButtonPushed:
                if self.window is not None:
                    xp.hideWidget(self.window)
                return 1
            if inMessage == xp.Msg_ButtonStateChanged:
                for key, cb in self.categoryCheckbox.items():
                    if inParam1 == cb:
                        state = 1 if xp.getWidgetProperty(cb, xp.Property_ButtonState) == 1 else 0
                        self.values[key] = state
                        self.profile = PROFILE_CUSTOM
                        self.SaveCustom()
                        self.UpdateChecks()   # refreshes the top-menu radios
                        _log("custom window: %s=%d (profile Custom)" % (key, state))
                        return 1
        except Exception:
            _log("WindowCallback error:\n" + traceback.format_exc())
        return 0

    def SelectProfile(self, profile):
        self.profile = profile
        self.ApplyProfileToValues()
        if profile == PROFILE_AUTO:
            self.ClearSaved()          # Auto is never persisted
        else:
            self.SaveNamed(profile)    # drops any custom categories
        self.UpdateChecks()

    def UpdateChecks(self):
        if not self.menuCreated or self.menuID is None:
            return
        try:
            # Every profile item (incl. Custom) is a plain level-1 item now, so all are
            # safe to checkmark - Custom shows checked whenever the profile is Custom.
            for refCon, label, value in ProfileItems:
                if refCon in self.profileItemIndex:
                    state = MenuChecked if value == self.profile else MenuUnchecked
                    xp.checkMenuItem(self.menuID, self.profileItemIndex[refCon], state)
            self.RefreshWindowCheckboxes()
        except Exception:
            _log("UpdateChecks error:\n" + traceback.format_exc())

    # ---- per-livery persistence ----
    def CurrentLiveryKey(self):
        try:
            fileName, path = xp.getNthAircraftModel(0)
        except Exception:
            path = "?"
        liveryIndex = 0
        if self.liveryIndexRef is not None:
            try:
                liveryIndex = xp.getDatai(self.liveryIndexRef)
            except Exception:
                liveryIndex = 0
        return "%s#%d" % (path or "?", liveryIndex)

    def ProfilesFilePath(self):
        try:
            root = xp.getSystemPath()
        except Exception:
            root = ""
        return os.path.join(root, "Output", "preferences", "ToLissPhoton_profiles.json")

    def LoadProfilesFile(self):
        self.profiles = {}
        try:
            with open(self.ProfilesFilePath(), "r") as f:
                data = json.load(f)
            if isinstance(data, dict):
                self.profiles = self._migrate(data)
        except Exception:
            self.profiles = {}

    def _migrate(self, data):
        """Accept the new per-livery dict schema; migrate legacy bare-int entries
        (old scheme: 0 Auto, 1 LED, 2 Classic)."""
        out = {}
        for key, value in data.items():
            if isinstance(value, dict):
                out[key] = value
                continue
            old = None
            if isinstance(value, bool):
                old = None
            elif isinstance(value, int):
                old = value
            elif isinstance(value, str) and value.strip().lstrip("-").isdigit():
                old = int(value)
            if old == 1:
                out[key] = {"profile": PROFILE_FULL_LED}
            elif old == 2:
                out[key] = {"profile": PROFILE_CLASSIC}
            # old 0 (Auto) or anything else -> no entry (absence == Auto)
        return out

    def SaveProfilesFile(self):
        path = self.ProfilesFilePath()
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                json.dump(self.profiles, f, indent=2)
        except Exception as e:
            _log("could not save profiles: %s" % e)

    def LoadProfileForCurrentLivery(self):
        key = self.CurrentLiveryKey()
        entry = self.profiles.get(key)
        if not isinstance(entry, dict):
            self.profile = PROFILE_AUTO
            self.ApplyProfileToValues()
        else:
            try:
                self.profile = int(entry.get("profile", PROFILE_AUTO))
            except Exception:
                self.profile = PROFILE_AUTO
            if self.profile == PROFILE_CUSTOM:
                cats = entry.get("categories", {})
                self.values = {k: (1 if int(cats.get(k, 0)) == 1 else 0) for k in CategoryKeys}
            else:
                self.ApplyProfileToValues()
        self.UpdateChecks()

    def SaveNamed(self, profile):
        key = self.CurrentLiveryKey()
        self.profiles[key] = {"profile": int(profile)}
        self.SaveProfilesFile()

    def SaveCustom(self):
        key = self.CurrentLiveryKey()
        self.profiles[key] = {"profile": PROFILE_CUSTOM, "categories": dict(self.values)}
        self.SaveProfilesFile()

    def ClearSaved(self):
        key = self.CurrentLiveryKey()
        if key in self.profiles:
            del self.profiles[key]
            self.SaveProfilesFile()
