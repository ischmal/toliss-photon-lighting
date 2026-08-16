#include "installer/screens.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/actions.h"
#include "core/config.h"
#include "core/constants.h"
#include "core/detect.h"
#include "core/fsutil.h"
#include "core/payload.h"
#include "core/version.h"
#include "core/wingmod.h"
#include "installer/log.h"
#include "installer/platform.h"
#include "installer/tui.h"

namespace photon {
namespace installer {
namespace {

namespace C = tui::C;

// Breadcrumb stages, matching tui::Stages().
constexpr int kStageRoot = 0;
constexpr int kStageAircraft = 1;
constexpr int kStageConfigure = 2;
constexpr int kStageRun = 3;
constexpr int kStageComplete = 4;

// Cosmetic per-step reveal delay on the Perform screen.
constexpr int kStepDelayMs = 100;
// Cadence of the animated "Working…" ellipsis while the (blocking) filesystem
// work runs on a worker thread — short enough to read as live, not frozen.
constexpr int kWorkingAnimMs = 300;
// How often the wait screen rechecks whether X-Plane is still up.
constexpr double kPollIntervalSeconds = 2.0;

FileLog* gLog = nullptr;
bool gDryRun = false;

// ─── small helpers ───────────────────────────────────────────────────────────
std::size_t Utf8Next(const std::string& s, std::size_t i) {
    if (i >= s.size()) return s.size();
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    return std::min(s.size(), i + len);
}

// Left-justify to `width` codepoints, or truncate with an ellipsis if longer.
// ⚠ Codepoints, not bytes: an aircraft folder named in any language must not be
// cut mid-character, which would put a broken byte sequence on the screen.
std::string Fit(const std::string& text, int width) {
    if (width < 1) return std::string();
    const int len = tui::VisLen(text);
    if (len <= width) {
        return text + std::string(static_cast<std::size_t>(width - len), ' ');
    }
    std::size_t i = 0;
    for (int n = 0; n < width - 1 && i < text.size(); ++n) i = Utf8Next(text, i);
    return text.substr(0, i) + "…";
}

std::string Paren(const std::string& label, const std::string& note) {
    return label + "  " + C::kBrBlack + "(" + note + ")" + C::kReset;
}

void LogLine(const std::string& msg, const std::string& level = "INFO") {
    if (gLog != nullptr) gLog->Write(msg, level);
}

// ─── screen 1: launch ────────────────────────────────────────────────────────
const std::vector<std::string>& LogoArt() {
    static const std::vector<std::string> kArt = {
        R"(    T o L i s s  ====================)",
        R"(    ____  __          __)",
        R"(   / __ \/ /_  ____  / /_____  ____)",
        R"(  / /_/ / __ \/ __ \/ __/ __ \/ __ \)",
        R"( / ____/ / / / /_/ / /_/ /_/ / / / /)",
        R"(/_/   /_/ /_/\____/\__/\____/_/ /_/)",
        "",
        std::string(" =========================  v") + kPhotonVersion,
    };
    return kArt;
}

// Two columns, aligned row for row: the cyan ASCII logo on the left and the
// welcome blurb on the right, the whole block horizontally centred.
std::vector<std::string> LaunchBody() {
    constexpr int kLeftW = 48;
    const std::vector<std::string>& art = LogoArt();
    const std::vector<std::string> blurb = {
        std::string(C::kBrBlack) + "ToLiss Photon v" + kPhotonVersion + C::kReset,
        "",
        std::string(C::kBrWhite) + "Welcome to the ToLiss Photon Installer!" + C::kReset,
        "",
        std::string(C::kWhite) + "This is a lighting modification program for" + C::kReset,
        std::string(C::kWhite) + "ToLiss aircraft in X-Plane 12." + C::kReset,
        "",
        tui::PressHint("any key", "continue…"),
    };

    std::vector<std::string> out;
    const std::size_t rows = std::min(art.size(), blurb.size());
    for (std::size_t i = 0; i < rows; ++i) {
        const std::string left =
            art[i].empty() ? art[i] : std::string(C::kBrCyan) + art[i] + C::kReset;
        if (blurb[i].empty()) {
            out.push_back(left);
            continue;
        }
        const int pad = std::max(1, kLeftW - tui::VisLen(left));
        out.push_back(left + std::string(static_cast<std::size_t>(pad), ' ') + blurb[i]);
    }

    // Keep the block's own content width but offset every row by a common left pad
    // so the whole thing sits mid-terminal. The columns are not equal, so the
    // midpoint is taken from the widest row.
    int width = 0;
    int height = 0;
    tui::Dims(width, height);
    const int contentW = width - tui::kBodyIndent;
    int blockW = 0;
    for (const std::string& l : out) blockW = std::max(blockW, tui::VisLen(l));
    const int off = std::max(0, (contentW - blockW) / 2);
    if (off > 0) {
        for (std::string& l : out) {
            if (!l.empty()) l = std::string(static_cast<std::size_t>(off), ' ') + l;
        }
    }
    return out;
}

int Quit();   // forward: the launch screen's ESC exits immediately

void ScreenLaunch() {
    LogLine("launch screen shown");
    tui::RenderOpts o;
    o.back = "exit";
    o.cont = "continue";
    o.contKey = "ANY KEY";
    o.center = true;
    tui::Render(tui::kNoStage, LaunchBody(), o);
    const std::string k = tui::ReadKey();
    if (k == tui::kKeyEsc || k == tui::kKeyInterrupt) std::exit(Quit());
}

// ─── screen 2: X-Plane directory ─────────────────────────────────────────────
const char kStepXplane[] = "Step 1.  Where is X-Plane installed?";

// Undo the quoting a drag-and-drop of a folder onto the console often adds
// (Windows and macOS both wrap a dropped path containing spaces in double quotes;
// some shells use single quotes) before treating it as a path — otherwise the
// literal quote characters end up part of the path and it never validates.
std::string CleanManualPath(const std::string& raw) {
    std::string s = fsutil::Trim(raw);
    if (s.size() >= 2 && s.front() == s.back() && (s.front() == '"' || s.front() == '\'')) {
        s = fsutil::Trim(s.substr(1, s.size() - 2));
    }
    return s;
}

std::string ScreenXplane() {
    std::vector<std::string> roots;
    for (const detect::RootCandidate& c : detect::XplaneCandidates()) {
        roots.push_back(c.path);
    }

    // ⚠ A REMEMBERED ROOT IS A HINT, NEVER A FACT. It may have been moved,
    // renamed or deleted since; revalidate before offering it, and forget it when
    // it no longer resolves so the user is not shown a dead path every run.
    std::string remembered = config::LoadSavedRoot();
    if (!remembered.empty()) {
        std::error_code ec;
        const std::filesystem::path canon =
            std::filesystem::weakly_canonical(fsutil::PathFromUtf8(remembered), ec);
        if (!ec) remembered = fsutil::PathToUtf8(canon);
        if (detect::IsValidRoot(remembered)) {
            LogLine("remembered X-Plane root still valid: " + remembered);
            if (std::find(roots.begin(), roots.end(), remembered) == roots.end()) {
                roots.insert(roots.begin(), remembered);
            }
        } else {
            LogLine("remembered X-Plane root no longer valid, discarding: " + remembered,
                    "WARN");
            config::ClearSavedRoot();
            remembered.clear();
        }
    }
    if (roots.empty()) LogLine("no X-Plane install auto-detected", "WARN");

    int defaultIdx = 0;
    if (!remembered.empty()) {
        const auto it = std::find(roots.begin(), roots.end(), remembered);
        if (it != roots.end()) defaultIdx = static_cast<int>(it - roots.begin());
    }

    while (true) {
        std::vector<std::string> options;
        for (const std::string& r : roots) {
            options.push_back(r == remembered
                                  ? r + "  " + C::kDim + "(last used)" + C::kReset
                                  : r);
        }
        options.push_back("Somewhere else…");

        tui::MenuOpts mo;
        mo.index = defaultIdx;
        const int choice = tui::Menu(kStageRoot, kStepXplane,
                                     "Please select your root X-Plane 12 folder:",
                                     options, mo);
        if (choice != static_cast<int>(roots.size())) {
            LogLine("selected X-Plane: " + roots[static_cast<std::size_t>(choice)]);
            return roots[static_cast<std::size_t>(choice)];
        }

        std::string manual;
        std::string error;
        while (true) {
            try {
                manual = tui::TextPrompt(kStageRoot, kStepXplane,
                                         "Manually specify your X-Plane 12 folder below:",
                                         manual, error);
            } catch (const tui::Back&) {
                break;   // back to the root menu, same options
            }
            if (manual.empty()) {
                error.clear();
                continue;
            }
            const std::string p = CleanManualPath(manual);
            if (!detect::IsValidRoot(p)) {
                LogLine("manual X-Plane path rejected: " + manual, "WARN");
                error = "'" + p +
                        "' doesn't look like an X-Plane 12 folder — expected an "
                        "Aircraft/ and a Resources/ folder inside it.";
                continue;
            }
            LogLine("selected X-Plane (manual): " + p);
            config::SaveRoot(p);
            return p;
        }
    }
}

// ─── screen 3: aircraft selection (table) ────────────────────────────────────
const char* const kAcTitles[3] = {"Folder Name", "Aircraft", "Photon Version"};
constexpr int kAcGap = 4;

struct AcCells {
    std::string folder;
    std::string aircraft;
    std::string photonText;
    const char* photonColor;
};

// The Photon column is kept split so it can be MEASURED by its visible text yet
// RENDERED in its status color — measuring the colored string would count the
// escape bytes into the column width.
AcCells CellsFor(const detect::Aircraft& ac) {
    AcCells c;
    c.folder = ac.folder;
    c.aircraft = ac.name + " (v" + ac.acVer + ")";
    if (ac.stale) {
        // A reinstall always lays down THIS installer's version, so name the move
        // whenever it differs — matching the "Update available" row below.
        const std::string bump =
            ac.photon != kPhotonVersion ? std::string(" → v") + kPhotonVersion : "";
        c.photonText = "Needs reinstall (v" + ac.photon + bump + ")";
        c.photonColor = C::kBrYellow;
    } else if (!ac.photon.empty()) {
        const std::string wingNote =
            ac.wing.empty() ? std::string() : " (" + WingLabel(ac.wing) + ")";
        if (ac.photon != kPhotonVersion) {
            c.photonText = "Update available (v" + ac.photon + " → v" +
                           kPhotonVersion + ")" + wingNote;
            c.photonColor = C::kBrCyan;
        } else {
            c.photonText = "v" + ac.photon + wingNote;
            c.photonColor = C::kBrGreen;
        }
    } else {
        c.photonText = "Not installed";
        c.photonColor = C::kDim;
    }
    return c;
}

// Preferred: every column as wide as the widest, with kAcGap between them. If that
// overflows the terminal, natural per-column widths; if even those do not fit,
// squeeze the middle Aircraft column so the row still fits.
void AircraftColumns(const std::vector<detect::Aircraft>& aircraft, int& w1, int& w2,
                     int& w3) {
    w1 = tui::VisLen(kAcTitles[0]);
    w2 = tui::VisLen(kAcTitles[1]);
    w3 = tui::VisLen(kAcTitles[2]);
    for (const detect::Aircraft& ac : aircraft) {
        const AcCells c = CellsFor(ac);
        w1 = std::max(w1, tui::VisLen(c.folder));
        w2 = std::max(w2, tui::VisLen(c.aircraft));
        w3 = std::max(w3, tui::VisLen(c.photonText));
    }
    int width = 0;
    int height = 0;
    tui::Dims(width, height);
    const int avail = width - tui::kBodyIndent - tui::kPrefix;
    const int colw = std::max(w1, std::max(w2, w3));
    if (3 * colw + 2 * kAcGap <= avail) {
        w1 = w2 = w3 = colw;
        return;
    }
    if (w1 + w2 + w3 + 2 * kAcGap <= avail) return;
    w2 = std::max(6, avail - w1 - w3 - 2 * kAcGap);
}

std::vector<std::string> AircraftHeader(int w1, int w2, int w3) {
    const std::string gap(kAcGap, ' ');
    const std::string titles =
        Fit(kAcTitles[0], w1) + gap + Fit(kAcTitles[1], w2) + gap + kAcTitles[2];
    const int ruleW = w1 + w2 + w3 + 2 * kAcGap;
    const std::string pad(tui::kPrefix, ' ');
    std::string rule;
    for (int i = 0; i < ruleW; ++i) rule += "▔";
    return {pad + C::kDim + titles + C::kReset,
            pad + C::kBrBlack + rule + C::kReset};
}

std::string AircraftOption(const detect::Aircraft& ac, int w1, int w2) {
    const std::string gap(kAcGap, ' ');
    const AcCells c = CellsFor(ac);
    return Fit(c.folder, w1) + gap + Fit(c.aircraft, w2) + gap + c.photonColor +
           c.photonText + C::kReset;
}

detect::Aircraft ScreenAircraft(const std::string& xplaneRoot) {
    while (true) {
        const std::vector<detect::Aircraft> aircraft = detect::DetectAircraft(xplaneRoot);
        if (aircraft.empty()) {
            tui::Flash(kStageAircraft,
                       {std::string(C::kRed) + "No compatible ToLiss aircraft found under" +
                            C::kReset,
                        std::string(C::kDim) +
                            fsutil::PathToUtf8(fsutil::PathFromUtf8(xplaneRoot) /
                                               "Aircraft") +
                            C::kReset});
            throw tui::Back{};
        }
        int w1 = 0;
        int w2 = 0;
        int w3 = 0;
        AircraftColumns(aircraft, w1, w2, w3);
        std::vector<std::string> options;
        for (const detect::Aircraft& ac : aircraft) {
            options.push_back(AircraftOption(ac, w1, w2));
        }
        options.push_back(std::string(C::kBrBlack) + "Refresh list…" + C::kReset);

        tui::MenuOpts mo;
        mo.headerLines = AircraftHeader(w1, w2, w3);
        const int choice =
            tui::Menu(kStageAircraft, "Step 2.  Which aircraft do you wish to modify?",
                      "Select a supported aircraft:", options, mo);
        if (choice == static_cast<int>(aircraft.size())) {
            LogLine("user requested aircraft list refresh");
            continue;   // redo detection from scratch
        }
        LogLine("selected aircraft: " + aircraft[static_cast<std::size_t>(choice)].folder);
        return aircraft[static_cast<std::size_t>(choice)];
    }
}

// ─── screen 4: aircraft action ───────────────────────────────────────────────
std::string ActionLabel(const detect::Aircraft& ac, const std::string& wing) {
    const std::string base = WingActionLabel(wing);
    if (ac.photon.empty()) return base;
    const bool sameWing = ac.wing == wing || (wing == "stock" && ac.wing.empty());
    const std::string from =
        "switch from " + WingLabel(ac.wing.empty() ? "stock" : ac.wing);
    if (ac.stale) {
        // The OBJ marker is gone (an aircraft update reverted our edit) though the
        // manifest survived — the on-disk OBJ is stock, so this is a fresh redo.
        return sameWing ? Paren(base, "reinstall — update reverted it") : Paren(base, from);
    }
    if (sameWing) {
        if (ac.photon != kPhotonVersion) {
            return Paren("Reinstall / Upgrade",
                         "v" + ac.photon + " → v" + kPhotonVersion);
        }
        return Paren("Reinstall", std::string("already v") + kPhotonVersion);
    }
    return Paren(base, from);
}

// Which wing variant to preselect for a FRESH install, from what is actually
// drawing on this aircraft — the GUI's `SeedWing`, ported so a console user gets
// the same default (docs/installer.md §Wing-mod detection). Same three rules:
// it is a default the user can overrule (the menu is right there), it is clamped
// to what the airframe offers, and the verdict is logged because a preselected
// row explains itself to nobody.
//
// Synchronous, unlike the GUI's WingProbe: the check reads the `.acf` variants
// and both wing OBJs (~13 MB), which in a click handler is a window that does not
// repaint — but this caller renders a "checking" frame FIRST, and a terminal
// screen that says what it is doing is allowed to take the beat.
std::string SeedWing(const detect::Aircraft& ac) {
    const std::vector<std::string>& wings = WingsFor(ac.airframe);
    if (wings.size() <= 1) return "stock";
    // Answered once per aircraft, like the GUI's `done` map: ESC from a later
    // step re-enters this screen, and re-reading 13 MB to re-offer the same
    // default would put the "checking" beat on every Back press.
    static std::map<std::string, std::string> answered;
    const auto it = answered.find(ac.path);
    if (it != answered.end()) return it->second;
    tui::Render(kStageConfigure,
                {"", tui::Step("Step 3.  Checking the aircraft…"), "",
                 std::string(C::kDim) +
                     "Looking at which wing mod is actually installed…" + C::kReset});
    const wingmod::Result wm = wingmod::Detect(ac.path, ac.airframe);
    const std::string want = wm.determined ? wm.wing : std::string("stock");
    const bool offered = std::find(wings.begin(), wings.end(), want) != wings.end();
    const std::string seed = offered ? want : std::string("stock");
    LogLine("action: " + wm.summary + " - preselecting " + WingLabel(seed));
    answered[ac.path] = seed;
    return seed;
}

std::string ScreenAction(const detect::Aircraft& ac) {
    const std::string step = "Step 3.  What would you like to do to " + ac.folder +
                             " (" + ac.name + " v" + ac.acVer + ")?";
    const std::vector<std::string>& wings = WingsFor(ac.airframe);
    std::vector<std::string> keys = wings;
    keys.push_back("uninstall");

    std::vector<std::string> options;
    std::set<int> disabled;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == "uninstall") {
            if (ac.photon.empty()) {
                options.push_back(Paren("Uninstall", "nothing installed"));
                disabled.insert(static_cast<int>(i));
            } else {
                options.push_back("Uninstall");
            }
        } else {
            options.push_back(ActionLabel(ac, keys[i]));
        }
    }

    // With Photon already installed, default the highlight to the currently-
    // installed wing variant so a reinstall lands on the obvious choice; a fresh
    // install defaults to what the wing-mod probe says is actually drawing —
    // the same seed the GUI's Features screen opens on.
    int defaultIdx = 0;
    const std::string w = !ac.photon.empty()
                              ? (ac.wing.empty() ? std::string("stock") : ac.wing)
                              : SeedWing(ac);
    {
        const auto it = std::find(wings.begin(), wings.end(), w);
        if (it != wings.end()) defaultIdx = static_cast<int>(it - wings.begin());
    }

    tui::MenuOpts mo;
    mo.disabled = disabled;
    mo.index = defaultIdx;
    const int choice =
        tui::Menu(kStageConfigure, step,
                  std::string("Select an action for ToLiss Photon v") + kPhotonVersion +
                      ":",
                  options, mo);
    const std::string action = keys[static_cast<std::size_t>(choice)];
    LogLine("action chosen: " + action + " on " + ac.folder);
    return action;
}

// ─── the X-Plane-running guard ───────────────────────────────────────────────
// Hold before performing the run until X-Plane is closed, then proceed on its own
// — but ONLY when this run needs to replace the loaded plugin binary. Overwriting
// the aircraft OBJs mid-session is safe (X-Plane loads them into memory and
// releases the files); a loaded `.xpl` is memory-mapped and locked, so it is the
// one artifact that cannot be swapped while the sim is up.
void ScreenCheckNotRunning(const std::string& action, const std::string& xplaneRoot) {
    // ⚠ A DRY RUN WRITES NOTHING AT ALL, so there is no locked file to collide
    // with — and a dry run is exactly what you do WHILE the sim is open. The GUI
    // skips the wait for the same reason (gui.cpp, WaitForXplaneToClose), and
    // this gate used to be the one place the two front-ends disagreed.
    if (gDryRun) {
        LogLine("dry run - skipping the close-X-Plane wait (nothing is written)");
        return;
    }
    // ⚠ RENDER BEFORE THE COMPARISON. `PluginIsCurrent` reads two files
    // byte-for-byte and can take a beat on a network drive; without this the
    // previous screen sits frozen with no feedback and reads as a hang.
    tui::Render(kStageRun,
                {"", tui::Step("Step 3b.  Checking installed plugin…"), "",
                 std::string(C::kDim) +
                     "Comparing the installed plugin against this release…" + C::kReset});
    if (action != "uninstall" && actions::PluginIsCurrent(xplaneRoot)) {
        // ⚠ NO PROCESS CHECK HERE ON PURPOSE. It would only refine a log message,
        // and enumerating processes costs real time — the bulk of what made this
        // screen feel slow. Whether or not X-Plane is up, this run only rewrites
        // OBJs, so the wait is skipped either way.
        LogLine("plugin already current — this run only rewrites OBJs (safe even "
                "while X-Plane is running); skipping the close-X-Plane wait");
        return;
    }

    bool warned = false;
    int tick = 0;
    while (detect::XplaneRunning()) {
        if (!warned) {
            LogLine("X-Plane appears to be running — waiting for it to close", "WARN");
            warned = true;
        }
        const std::string dots(static_cast<std::size_t>(1 + tick % 3), '.');
        std::vector<std::string> body = {
            "",
            tui::Step("Step 3b.  Please close X-Plane to continue"),
            "",
            std::string(C::kYellow) + C::kBold + "X-Plane appears to be running." +
                C::kReset,
            "",
            std::string(C::kBrWhite) +
                "This install adds or updates the ToLiss Photon plugin, which X-Plane "
                "loads at startup — X-Plane keeps that file locked while it's open, so "
                "the install can't complete until it's closed." + C::kReset,
            "",
            std::string(C::kBrWhite) +
                "Close X-Plane and the installer will continue automatically." + C::kReset,
            "",
            std::string(C::kDim) + "Waiting for X-Plane to close" + dots + C::kReset,
        };
        tui::RenderOpts o;
        o.back = "go back";
        o.cont = "continue anyway";
        o.focus = static_cast<int>(body.size()) - 1;
        tui::Render(kStageRun, body, o);

        const std::string k = tui::ReadKeyTimeout(kPollIntervalSeconds);
        if (k == tui::kKeyEsc) {
            LogLine("user backed out of the X-Plane-running wait");
            throw tui::Back{};
        }
        if (k == tui::kKeyEnter) {
            LogLine("user chose to continue with X-Plane apparently running", "WARN");
            return;
        }
        ++tick;
    }
    if (warned) LogLine("X-Plane closed — installer continuing automatically");
}

// ─── the two sub-prompts ─────────────────────────────────────────────────────
// Opt in to the interior ("Gus Mod") cockpit lighting — a separate axis from the
// exterior mod, so a user who wants only the exterior can decline.
//
// Skipped entirely when the airframe is out of scope (the A330-900) or this build
// carries no interior payload, rather than offering a step that would fail.
bool ScreenInterior(const detect::Aircraft& ac) {
    if (!AirframeHasInterior(ac.airframe) || !payload::InteriorAvailable()) {
        LogLine("interior mod not offered for " + ac.airframe +
                " (unsupported airframe or no payload)");
        return false;
    }
    const std::vector<std::string> options = {
        "Yes (recommended)",
        "No",
        "Open X-Plane.org page…",
    };
    const std::string body =
        "ToLiss Photon includes Light Mod by Gus Rodrigues, offering improved cockpit "
        "lighting textures and three interior light profiles: Old Halogen, New "
        "Halogen, and LED.";

    int idx = 0;
    std::string note;
    while (true) {
        tui::MenuOpts mo;
        mo.index = idx;
        if (!note.empty()) mo.footerLines = {"", note};
        const int choice =
            tui::Menu(kStageConfigure,
                      "Step 3d.  (Optional) Install enhanced cockpit lighting?", body,
                      options, mo);
        if (choice == 2) {
            const std::string url = InteriorOrgUrl(ac.airframe);
            platform::OpenUrl(url);
            LogLine("opened the Light Mod page for " + ac.airframe + ": " + url);
            idx = 2;
            note = std::string(C::kBrBlack) + "Opened " + url + " in your browser." +
                   C::kReset;
            continue;
        }
        LogLine(std::string("interior opt-in: ") + (choice == 0 ? "true" : "false"));
        return choice == 0;
    }
}

// Whether to also remove the shared plugin on uninstall. Left installed by default
// (other airframes may use it); only offered when this is the last airframe with
// Photon on it.
bool ScreenPluginDisposition(const detect::Aircraft& ac, const std::string& xplaneRoot) {
    if (detect::AnyOtherAirframeHasPhoton(xplaneRoot, ac.path)) {
        LogLine("other airframes still have Photon — leaving shared plugin in place");
        return false;
    }
    const std::vector<std::string> options = {"Leave the plugin installed (recommended)",
                                              "Remove it too"};
    return tui::Menu(kStageConfigure, "Step 3c.  No other aircraft use ToLiss Photon",
                     "The ToLiss Photon plugin is shared across aircraft. Remove it too?",
                     options) == 1;
}

// ─── screen 5: perform ───────────────────────────────────────────────────────
// ⚠ THE PROGRESS STRING IS SHARED ACROSS THREADS AND MUST BE LOCKED. The Python
// original leans on GIL atomicity for a dict assignment; C++ has no such
// guarantee, and an unsynchronized std::string is a torn read waiting to happen.
struct Progress {
    std::mutex mu;
    std::string text;

    void Set(const std::string& s) {
        std::lock_guard<std::mutex> lock(mu);
        text = s;
    }
    std::string Get() {
        std::lock_guard<std::mutex> lock(mu);
        return text;
    }
};

// ⚠ THE TERMINAL INSTALLER GAINS THE PERCENTAGE FOR FREE. It only ever said
// "copying textures 7/11" — during the one loop that reported, on the one axis that
// is opt-in — so its spinner was as uninformative as the GUI's bar. The plan now
// covers every phase and names it, and a text line can show both without needing a
// design change the way the GUI's does.
void OnProgress(double fraction, const std::string& step, void* userData) {
    const int pct = static_cast<int>(fraction * 100.0 + 0.5);
    static_cast<Progress*>(userData)->Set(std::to_string(pct) + "% — " + step);
}

std::string VariantLabel(const std::string& action, bool interior) {
    return WingLabel(action) + (interior ? " + Cockpit" : "");
}

struct PerformResult {
    bool success = false;
    bool interior = false;
};

PerformResult ScreenPerform(const detect::Aircraft& ac, const std::string& action,
                            const std::string& xplaneRoot) {
    const std::string verb = action == "uninstall" ? "Uninstalling…" : "Installing…";
    const std::string title = tui::Step("Step 4.  " + verb);

    // ⚠ ANY INTERACTIVE PROMPT RUNS ON THE MAIN THREAD BEFORE THE SPINNER OWNS IT
    // — the animation loop below reads no keys. ScreenPluginDisposition may throw
    // Back; let it propagate to the flow unchanged.
    bool removePlugin = false;
    bool wantInterior = false;
    if (action == "uninstall") {
        removePlugin = ScreenPluginDisposition(ac, xplaneRoot);
    } else {
        wantInterior = ScreenInterior(ac);
    }

    actions::Options opts;
    opts.airframeKey = ac.airframe;
    opts.aircraftPath = ac.path;
    opts.wing = action == "uninstall" ? std::string("stock") : action;
    opts.xplaneRoot = xplaneRoot;
    opts.interior = wantInterior;
    opts.dryRun = gDryRun;
    opts.removePlugin = removePlugin;

    Progress progress;
    opts.progress = &OnProgress;
    opts.progressUserData = &progress;

    std::vector<std::string> steps;
    std::string error;
    std::exception_ptr unexpected;

    // The real filesystem work is synchronous and can take a beat (backing up OBJs,
    // RealWings/glow patching, the plugin copy, ~57 MiB of interior textures),
    // during which a static "Working…" looks hung. Run it on a worker and animate
    // on the main thread so the screen visibly stays alive.
    std::future<void> worker = std::async(std::launch::async, [&]() {
        try {
            steps = action == "uninstall" ? actions::Uninstall(opts, *gLog)
                                          : actions::Install(opts, *gLog);
        } catch (const actions::ActionError& e) {
            error = e.what();
            LogLine(std::string("ACTION FAILED: ") + e.what(), "ERROR");
        } catch (const std::exception& e) {
            error = std::string("unexpected error: ") + e.what();
            LogLine(std::string("ACTION FAILED: ") + e.what(), "ERROR");
        } catch (...) {
            // ⚠ NEVER LET THE WORKER VANISH SILENTLY — that would render as a
            // (false) success. Captured here and rethrown on the main thread.
            unexpected = std::current_exception();
        }
    });

    int tick = 0;
    while (true) {   // renders at least once, then animates until the work finishes
        std::string dots(static_cast<std::size_t>(1 + tick % 3), '.');
        dots.resize(3, ' ');
        std::vector<std::string> body = {"", title, "",
                                         std::string("  ") + C::kDim + "Working" + dots +
                                             C::kReset};
        // The interior step copies ~57 MiB of textures; naming the current file is
        // what keeps that from reading as a hang.
        const std::string note = progress.Get();
        if (!note.empty()) {
            body.push_back("");
            body.push_back(std::string("  ") + C::kBrBlack + note + C::kReset);
        }
        tui::Render(kStageRun, body);
        if (worker.wait_for(std::chrono::milliseconds(kWorkingAnimMs)) ==
            std::future_status::ready) {
            break;
        }
        ++tick;
    }
    worker.get();
    if (unexpected) std::rethrow_exception(unexpected);

    // Reveal each completed step one at a time so the run reads as a deliberate,
    // step-by-step process instead of an instant flash. Purely cosmetic — the
    // filesystem work already finished above; this only paces the display.
    std::vector<std::string> lines{""};
    for (const std::string& s : steps) {
        const bool warn = !s.empty() && s[0] == '!';
        const std::string marker = warn ? std::string(C::kYellow) + "!" + C::kReset
                                        : std::string(C::kBrGreen) + "✓" + C::kReset;
        std::string text = s;
        const std::size_t a = text.find_first_not_of("! ");
        text = a == std::string::npos ? std::string() : text.substr(a);
        lines.push_back("  " + marker + " " + text);
        std::vector<std::string> frame{"", title};
        frame.insert(frame.end(), lines.begin(), lines.end());
        tui::Render(kStageRun, frame);
        if (!gDryRun) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStepDelayMs));
        }
    }

    if (!error.empty()) {
        lines.push_back("");
        lines.push_back(std::string("  ") + C::kRed + C::kBold + "Failed: " + error +
                        C::kReset);
    } else {
        const std::string summary =
            action == "uninstall"
                ? "Uninstall complete — original ToLiss lighting restored."
                : "Install complete — " + ac.name + " now uses ToLiss Photon v" +
                      kPhotonVersion + " (" + VariantLabel(action, wantInterior) + ").";
        lines.push_back("");
        lines.push_back(std::string("  ") + C::kBrGreen + summary + C::kReset);
        if (action == "uninstall") {
            lines.push_back("");
            lines.push_back(std::string("  ") + C::kDim +
                            "Tip: if any lighting issues linger, reinstall the aircraft "
                            "with the" + C::kReset);
            lines.push_back(std::string("  ") + C::kDim +
                            "SkunkCrafts updater — it will redownload and replace all "
                            "modified files." + C::kReset);
        }
    }
    if (gDryRun) {
        lines.push_back(std::string("  ") + C::kYellow +
                        "(--dry-run — no files were actually modified)" + C::kReset);
    }

    std::vector<std::string> frame{"", title};
    frame.insert(frame.end(), lines.begin(), lines.end());
    tui::RenderOpts o;
    o.cont = "continue";
    o.focus = static_cast<int>(lines.size()) + 2;
    tui::Render(kStageRun, frame, o);
    while (true) {
        const std::string k = tui::ReadKey();
        if (k == tui::kKeyEnter) break;
        if (k == tui::kKeyInterrupt) std::exit(Quit());
    }

    PerformResult r;
    r.success = error.empty();
    r.interior = wantInterior;
    return r;
}

// ─── screen 6: complete ──────────────────────────────────────────────────────
std::string CompleteSummary(const detect::Aircraft& ac, const std::string& action,
                            bool success, bool interior) {
    if (!success) {
        const std::string verb = action == "uninstall" ? "Uninstall" : "Installation";
        return std::string(C::kYellow) + verb +
               " did not complete — see the installer log for details." + C::kReset;
    }
    if (action == "uninstall") {
        return std::string(C::kBrWhite) + "ToLiss Photon removed from " + ac.name +
               " — original lighting restored." + C::kReset;
    }
    return std::string(C::kBrWhite) + "ToLiss Photon v" + kPhotonVersion + " (" +
           VariantLabel(action, interior) + ") installed in " + ac.name + "." + C::kReset;
}

// Returns "exit" or "again".
std::string ScreenComplete(const detect::Aircraft& ac, const std::string& action,
                           bool success, bool interior) {
    std::string title;
    if (!success) {
        title = std::string("Step 5.  ") +
                (action == "uninstall" ? "Uninstall" : "Installation") + " failed";
    } else if (action == "uninstall") {
        title = "Step 5.  Uninstall complete";
    } else {
        title = "Step 5.  Installation complete";
    }
    const std::string summary = CompleteSummary(ac, action, success, interior);
    const std::vector<std::string> options = {"Exit installer", "Modify another aircraft",
                                              "View installer log…",
                                              "Open X-Plane.org Page…",
                                              "Open GitHub Project…"};
    std::string note;
    while (true) {
        // ⚠ NO BACK ON THIS SCREEN. The install/uninstall it reports on already
        // happened, so there is no previous step to rewind to — and ESC here must
        // not restart the whole installer, which is what a plain `allowBack` would
        // effectively do.
        tui::MenuOpts mo;
        mo.headerLines = {summary, "", "What would you like to do next:", ""};
        if (!note.empty()) mo.footerLines = {"", note};
        mo.allowBack = false;
        const int choice = tui::Menu(kStageComplete, title, "", options, mo);
        note.clear();
        if (choice == 0) return "exit";
        if (choice == 1) return "again";
        if (choice == 2) {
            LogLine("user opened installer log");
            const bool ok = platform::OpenInEditor(gLog->Path());
            note = ok ? std::string(C::kBrBlack) + "Viewed installer log: " +
                            gLog->Path() + C::kReset
                      : std::string(C::kRed) + "Could not open the installer log: " +
                            gLog->Path() + C::kReset;
        } else if (choice == 3) {
            platform::OpenUrl(kXplaneOrgUrl);
            note = std::string(C::kBrBlack) +
                   "Opened the X-Plane.org page in your browser." + C::kReset;
        } else if (choice == 4) {
            platform::OpenUrl(kGithubUrl);
            note = std::string(C::kBrBlack) + "Opened the GitHub project in your browser." +
                   C::kReset;
        }
    }
}

// ─── exit ────────────────────────────────────────────────────────────────────
// ⚠ THE ORDERING IS THE CONTRACT: cursor back on, THEN leave the alt screen, then
// print. Reversed, the closing lines land on the alt screen and vanish with it.
int Quit() {
    tui::ShowCursor();
    tui::LeaveAltScreen();
    std::cout << C::kDim << "ToLiss Photon installer closed." << C::kReset << "\n";
    if (gLog != nullptr) {
        std::cout << C::kDim << "Log saved to: " << gLog->Path() << C::kReset << "\n";
    }
    std::cout.flush();
    return 0;
}

void QuitHookFn() { Quit(); }

}  // namespace

int RunTui(const std::string& xplaneRootOverride, bool dryRun) {
    gDryRun = dryRun;
    FileLog log(kPhotonVersion, dryRun);
    gLog = &log;

    if (payload::Available()) {
        std::string msg = "payload: " + payload::PayloadDir();
        if (!payload::RealWingsAvailable()) {
            msg += " (no realwings_patch.json — the RealWings wingtip patch will be "
                   "skipped)";
        }
        log.Write(msg);
    } else {
        // ⚠ There is no dev-tree fallback that builds this on the fly — hence the
        // staging command in the message rather than just "build a release".
        log.Write("no installable payload found (looked in " + payload::PayloadDir() +
                      "); install/uninstall will fail until one is staged — "
                      "python build/make_release.py --payload-only",
                  "ERROR");
    }

    platform::EnableVt();
    tui::SetQuitHook(&QuitHookFn);
    tui::InstallInterruptHandler();
    tui::EnterAltScreen();
    tui::HideCursor();

    int rc = 0;
    try {
        ScreenLaunch();
        std::string xplaneRoot = xplaneRootOverride;
        bool haveRoot = !xplaneRoot.empty();
        while (true) {
            // ─ Step 1: X-Plane root. ESC here EXITS: there is no earlier real step
            // to fall back to (the launch screen has its own ESC handling).
            if (!haveRoot) {
                try {
                    xplaneRoot = ScreenXplane();
                    haveRoot = true;
                } catch (const tui::Back&) {
                    break;
                }
            }

            // ─ Step 2: aircraft selection. ⚠ ESC here is the ONE place that steps
            // all the way back to Step 1 — a different root has different aircraft
            // under it, so re-choosing the aircraft alone would be a dead end.
            detect::Aircraft ac;
            try {
                ac = ScreenAircraft(xplaneRoot);
            } catch (const tui::Back&) {
                haveRoot = false;
                xplaneRoot.clear();
                continue;
            }

            // ─ Step 3+: configure / run / complete for this aircraft. Every ESC
            // from here steps back exactly ONE stage, never to Step 1.
            while (true) {
                std::string action;
                try {
                    action = ScreenAction(ac);
                } catch (const tui::Back&) {
                    break;   // → Step 2 (aircraft selection), same root
                }
                try {
                    ScreenCheckNotRunning(action, xplaneRoot);
                } catch (const tui::Back&) {
                    continue;   // → re-choose the action, still Step 3
                }
                PerformResult res;
                try {
                    res = ScreenPerform(ac, action, xplaneRoot);
                } catch (const tui::Back&) {
                    continue;   // backed out of a sub-prompt → re-choose the action
                }
                if (ScreenComplete(ac, action, res.success, res.interior) == "exit") {
                    rc = Quit();
                    gLog = nullptr;
                    return rc;
                }
                break;   // "again" → Step 2 (aircraft selection), same root
            }
        }
        rc = Quit();
    } catch (const std::exception& e) {
        tui::ShowCursor();
        tui::LeaveAltScreen();
        log.Write(std::string("FATAL: ") + e.what(), "ERROR");
        std::cerr << "The installer hit an unexpected error: " << e.what() << "\n"
                  << "Log saved to: " << log.Path() << "\n";
        rc = 1;
    }
    gLog = nullptr;
    return rc;
}

}  // namespace installer
}  // namespace photon
