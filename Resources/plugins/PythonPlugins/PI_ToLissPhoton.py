"""
PI_ToLissPhoton.py  -  XPPython3 plugin for ToLiss Photon Lighting

Owns the plumbing the FlyWithLua script (ToLissPhoton.lua) can't do reliably on its
own, and (new) all of the profile-resolution logic:

  * Registers NINE per-category light-type datarefs at SIM STARTUP (so the aircraft
    OBJ can bind them before it loads). Each is 0 = halogen/xenon, 1 = LED:
        ToLissPhoton/exterior/{taxi,to,rwyturnoff,wing,landing,beacon,nav,strobe,logo}
    Plus a deprecated read-only alias ToLissPhoton/exterior/is_led = 1 only when ALL
    categories resolve LED, else 0 (safety net for any OBJ not yet migrated).

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
stale __pycache__ there and remove any older PI_PhotonMenu.py.
"""
import os
import json
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
MfrlGate          = ""      # when set AND present above threshold => Full LED (top priority)
MfrlThreshold     = 0.5
AutoFallbackProfile = PROFILE_CLASSIC   # used when no gate resolves


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
            _log("started; registered %d category datarefs" % len(self.accessors))
        except Exception:
            _log("XPluginStart error:\n" + traceback.format_exc())
        return self.Name, self.Sig, self.Desc

    def XPluginEnable(self):
        try:
            self.liveryIndexRef = xp.findDataRef(LiveryIndexDataRef)
            if not self.flightLoopRegistered:
                xp.registerFlightLoopCallback(self.FlightLoop, 1.0, 0)
                self.flightLoopRegistered = True
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
        except Exception:
            _log("XPluginDisable error:\n" + traceback.format_exc())

    def XPluginStop(self):
        try:
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
        """True/False if the gate dataref exists, else None (not configured/present)."""
        if not path:
            return None
        try:
            ref = xp.findDataRef(path)
            if ref is None:
                return None
            return xp.getDataf(ref) > threshold
        except Exception:
            try:
                return xp.getDatai(ref) > threshold
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
