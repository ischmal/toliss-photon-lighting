"""
PI_ToLissPhoton.py  -  XPPython3 plugin for ToLiss Photon Lighting

Owns the plumbing the FlyWithLua script (toliss_photon_lighting.lua) can't do
reliably on its own:

  * Registers two datarefs at SIM STARTUP (so the aircraft OBJ can bind is_led
    before it loads - a FlyWithLua dataref is created too late for that):
        ToLissPhoton/exterior/profile   Int  0 = Auto, 1 = LED, 2 = Classic
                                             (selection; read-only to the sim)
        ToLissPhoton/exterior/is_led    Int  0 = classic, 1 = LED
                                             (the Lua script writes it; OBJ reads it)
  * Shows a "ToLiss Photon" menu (Auto / LED / Classic) ONLY while a ToLiss
    aircraft is loaded.
  * Saves/loads the selection PER LIVERY to
        <X-Plane>/Output/preferences/ToLissPhoton_profiles.json

The decision logic (Neo detection, resolving the selection into is_led) lives in
the Lua script, which reads 'profile' and writes 'is_led'.

Install: copy into Resources/plugins/PythonPlugins/ (requires XPPython3).
Remove any older PI_PhotonMenu.py.
"""
import os
import json
from XPPython3 import xp

ProfileDataRefName = "ToLissPhoton/exterior/profile"
IsLedDataRefName   = "ToLissPhoton/exterior/is_led"
LiveryIndexDataRef = "sim/aircraft/view/acf_livery_index"

# (item refCon, label, profile value)
ProfileItems = (
    ("ToLissPhoton_auto",    "Auto",    0),
    ("ToLissPhoton_led",     "LED",     1),
    ("ToLissPhoton_classic", "Classic", 2),
)

# constants with integer fallbacks in case names differ by version
MenuChecked     = getattr(xp, "Menu_Checked", 2)
MenuUnchecked   = getattr(xp, "Menu_Unchecked", 1)
MsgPlaneLoaded  = getattr(xp, "MSG_PLANE_LOADED", 102)
MsgLiveryLoaded = getattr(xp, "MSG_LIVERY_LOADED", 108)


class PythonInterface:
    def __init__(self):
        self.Name = "ToLiss Photon"
        self.Sig  = "toliss.photon.lighting.menu"
        self.Desc = "ToLiss Photon Lighting: profile menu, datarefs, and per-livery save."
        self.profile = 0            # backs the read-only 'profile' dataref
        self.isLed = 0              # backs the 'is_led' dataref (written by Lua)
        self.profileAccessor = None
        self.isLedAccessor = None
        self.liveryIndexRef = None
        self.profiles = {}          # liveryKey -> profile value
        self.menuCreated = False
        self.menuID = None
        self.pluginsMenuItem = None
        self.itemIndex = {}

    # ---- dataref accessor callbacks ----
    def _readProfileInt(self, refCon):   return self.profile
    def _readProfileFloat(self, refCon): return float(self.profile)
    def _readIsLedInt(self, refCon):     return self.isLed
    def _readIsLedFloat(self, refCon):   return float(self.isLed)
    def _writeIsLedInt(self, refCon, value):   self.isLed = int(value)
    def _writeIsLedFloat(self, refCon, value): self.isLed = int(round(value))

    # ---- lifecycle ----
    def XPluginStart(self):
        # register at startup so is_led exists before any aircraft OBJ binds it
        self.profileAccessor = xp.registerDataAccessor(
            ProfileDataRefName, readInt=self._readProfileInt, readFloat=self._readProfileFloat)
        self.isLedAccessor = xp.registerDataAccessor(
            IsLedDataRefName, readInt=self._readIsLedInt, readFloat=self._readIsLedFloat,
            writeInt=self._writeIsLedInt, writeFloat=self._writeIsLedFloat)
        self.LoadProfilesFile()
        return self.Name, self.Sig, self.Desc

    def XPluginEnable(self):
        self.liveryIndexRef = xp.findDataRef(LiveryIndexDataRef)
        # handle being enabled while already sitting in an aircraft
        self.UpdateMenuVisibility()
        self.LoadProfileForCurrentLivery()
        return 1

    def XPluginDisable(self):
        pass

    def XPluginStop(self):
        self.DestroyMenu()
        if self.isLedAccessor is not None:
            xp.unregisterDataAccessor(self.isLedAccessor); self.isLedAccessor = None
        if self.profileAccessor is not None:
            xp.unregisterDataAccessor(self.profileAccessor); self.profileAccessor = None

    def XPluginReceiveMessage(self, inFromWho, inMessage, inParam):
        # inParam == 0 -> the user's aircraft
        if inMessage == MsgPlaneLoaded and inParam == 0:
            self.UpdateMenuVisibility()
            self.LoadProfileForCurrentLivery()
        elif inMessage == MsgLiveryLoaded and inParam == 0:
            self.LoadProfileForCurrentLivery()

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

    def CreateMenu(self):
        self.pluginsMenuItem = xp.appendMenuItem(xp.findPluginsMenu(), "ToLiss Photon", 0)
        self.menuID = xp.createMenu("ToLiss Photon", xp.findPluginsMenu(),
                                    self.pluginsMenuItem, self.MenuHandler, 0)
        self.itemIndex = {}
        for refCon, label, value in ProfileItems:
            self.itemIndex[refCon] = xp.appendMenuItem(self.menuID, label, refCon)
        self.menuCreated = True
        self.UpdateChecks()

    def DestroyMenu(self):
        if self.menuID is not None:
            xp.destroyMenu(self.menuID)
            self.menuID = None
        if self.pluginsMenuItem is not None:
            try:
                xp.removeMenuItem(xp.findPluginsMenu(), self.pluginsMenuItem)
            except Exception:
                pass
            self.pluginsMenuItem = None
        self.menuCreated = False

    # ---- menu handling ----
    def MenuHandler(self, menuRefCon, itemRefCon):
        for refCon, label, value in ProfileItems:
            if itemRefCon == refCon:
                self.profile = value
                self.SaveProfileForCurrentLivery()
                break
        self.UpdateChecks()

    def UpdateChecks(self):
        if not self.menuCreated or self.menuID is None:
            return
        for refCon, label, value in ProfileItems:
            state = MenuChecked if value == self.profile else MenuUnchecked
            xp.checkMenuItem(self.menuID, self.itemIndex[refCon], state)

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
                self.profiles = data
        except Exception:
            self.profiles = {}

    def SaveProfilesFile(self):
        path = self.ProfilesFilePath()
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                json.dump(self.profiles, f, indent=2)
        except Exception as e:
            xp.log("ToLiss Photon: could not save profiles: %s" % e)

    def LoadProfileForCurrentLivery(self):
        key = self.CurrentLiveryKey()
        try:
            self.profile = int(self.profiles.get(key, 0))   # default Auto
        except Exception:
            self.profile = 0
        self.UpdateChecks()

    def SaveProfileForCurrentLivery(self):
        key = self.CurrentLiveryKey()
        self.profiles[key] = self.profile
        self.SaveProfilesFile()