// Detection: X-Plane root candidates, the aircraft scan, and Photon install-status
// probing. Pure filesystem work — no TUI, no XPLM.
//
// ⚠ AIRCRAFT ARE IDENTIFIED BY CONTENT, NOT BY FOLDER NAME OR LOCATION. Users
// rename the folder and drop installs in sub-hangars (`Aircraft/My Hangar/…`), so
// the order is: the SkunkCrafts cfg's `module` URL (the most stable machine id),
// then its display `name`, then the airframe-specific OBJ filename (intrinsic to
// the aircraft's own files), and only then the legacy folder-name glob.
#pragma once

#include <string>
#include <vector>

namespace photon {
namespace detect {

// ─── X-Plane root discovery ──────────────────────────────────────────────────
// Must contain both `Aircraft/` and `Resources/`.
bool IsValidRoot(const std::string& pathUtf8);

struct RootCandidate {
    std::string path;
    std::string why;     // "$XPLANE_ROOT", "steam library", …
};

// Every plausible X-Plane 12 root, de-duped and validated. Order:
// `$XPLANE_ROOT`, running-from-inside-X-Plane, Steam libraries, then the common
// per-OS install directories.
std::vector<RootCandidate> XplaneCandidates(const std::string& selfPathUtf8 = "");

// Steam library folders parsed out of `libraryfolders.vdf` — a tiny hand parser
// for the `"path"  "D:\\SteamLibrary"` lines, so the installer keeps its
// zero-third-party-dependency property.
std::vector<std::string> ParseLibraryVdf(const std::string& text);

// ⚠ NEVER RAISES, AND ANY FAILURE MEANS "NOT RUNNING". The check exists to warn,
// so it must not be able to block an install by failing to introspect processes.
bool XplaneRunning();

// Does one process name — a Windows image name, or one line of `ps -A -o comm=` —
// name the X-Plane binary? Split out of `XplaneRunning` so the matching itself is
// testable without processes; the platform half stays untestable by nature.
//
// ⚠ THIS USED TO BE `find("X-Plane") != npos` ON UNIX and that is a real bug, not
// a tidy-up (fixed 2026-08-15). `ps -o comm=` prints the FULL PATH on macOS, so
// every process running out of `/Applications/X-Plane 12/` matched — X-Plane's own
// updater, any helper, anything at all under that folder. It only ever produced a
// spurious warning while the TUI owned this check, because the TUI offers
// "continue anyway"; the GUI waits on it, so a false positive there is an install
// that can never start.
//
// Matches the three real binaries and nothing that merely lives beside them:
//   Windows  X-Plane.exe
//   macOS    …/X-Plane.app/Contents/MacOS/X-Plane
//   Linux    X-Plane-x86_64
bool IsXplaneProcessName(const std::string& raw);

// ─── the aircraft scan ───────────────────────────────────────────────────────
struct SkunkCfg {
    std::string module;
    std::string name;
    std::string version;   // leading v/V stripped
};
SkunkCfg ReadSkunkCfg(const std::string& folderUtf8);
SkunkCfg ParseSkunkCfg(const std::string& text);

// The airframe key, or empty when this is not one of ours.
std::string IdentifyAirframe(const std::string& folderUtf8);

// (version, wing, stale) for an aircraft's `objects/` folder.
//
// ⚠ THE OBJ MARKER CORROBORATES THE MANIFEST rather than merely backing it up.
// The manifest is authoritative for version and wing, but the marker decides
// whether the edit is still IN THE FILE: manifest present + marker gone means an
// aircraft update reverted the OBJ out from under us, i.e. STALE — the install
// needs redoing even though the bookkeeping survived.
struct Status {
    std::string version;
    std::string wing;
    bool installed = false;
    bool stale = false;
};
Status PhotonStatus(const std::string& objectsDirUtf8);

// ⚠ A SECOND, INDEPENDENT AXIS — deliberately not folded into PhotonStatus.
// That function scans only the exterior `lights_out` OBJs, so an aircraft with the
// interior installed and the exterior reverted would read as "not installed at
// all" and hide a live modification from the user.
Status InteriorStatus(const std::string& objectsDirUtf8);

struct Aircraft {
    std::string folder;     // path relative to Aircraft/, for a legible label
    std::string path;       // absolute
    std::string airframe;
    std::string acVer;
    std::string name;
    std::string photon;     // installed version, or empty
    std::string wing;
    bool stale = false;
};

std::vector<Aircraft> DetectAircraft(const std::string& xplaneRootUtf8);

// Used by the uninstall flow to decide the shared plugin's fate: leave it
// installed unless this is the last airframe carrying Photon. Compares on the full
// PATH, since nested layouts can have same-named folders in different hangars.
bool AnyOtherAirframeHasPhoton(const std::string& xplaneRootUtf8,
                               const std::string& excludePathUtf8);

}  // namespace detect
}  // namespace photon
