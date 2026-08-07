#include "installer/cli.h"

#include <iostream>
#include <map>
#include <set>

#include "core/actions.h"
#include "core/config.h"
#include "core/constants.h"
#include "core/detect.h"
#include "core/fsutil.h"
#include "core/manifest.h"
#include "core/patch_realwings.h"
#include "core/payload.h"
#include "core/version.h"
#include "third_party/nlohmann/json.hpp"

namespace photon {
namespace installer {
namespace {

using nlohmann::json;

const std::set<std::string>& Subcommands() {
    static const std::set<std::string> kCmds = {
        "detect", "status", "install", "uninstall", "version", "help",
    };
    return kCmds;
}

struct Args {
    std::string command;
    std::map<std::string, std::string> opts;
    std::set<std::string> flags;

    bool Has(const std::string& k) const { return flags.count(k) > 0; }
    std::string Get(const std::string& k, const std::string& fallback = "") const {
        const auto it = opts.find(k);
        return it == opts.end() ? fallback : it->second;
    }
};

// Flags that take no value. Everything else beginning `--` consumes the next
// argument (or the text after `=`).
const std::set<std::string>& ValuelessFlags() {
    static const std::set<std::string> kFlags = {
        "json", "dry-run", "interior", "remove-plugin", "force", "help",
    };
    return kFlags;
}

Args ParseArgs(const std::vector<std::string>& argv, std::string& errorOut) {
    Args a;
    std::size_t i = 1;
    if (i < argv.size() && !fsutil::StartsWith(argv[i], "-")) {
        a.command = argv[i];
        ++i;
    }
    for (; i < argv.size(); ++i) {
        std::string tok = argv[i];
        if (!fsutil::StartsWith(tok, "--")) {
            errorOut = "unexpected argument: " + tok;
            return a;
        }
        tok = tok.substr(2);
        std::string value;
        bool hasInlineValue = false;
        const std::size_t eq = tok.find('=');
        if (eq != std::string::npos) {
            value = tok.substr(eq + 1);
            tok = tok.substr(0, eq);
            hasInlineValue = true;
        }
        if (ValuelessFlags().count(tok) > 0) {
            if (hasInlineValue) {
                errorOut = "--" + tok + " takes no value";
                return a;
            }
            a.flags.insert(tok);
            continue;
        }
        if (!hasInlineValue) {
            if (i + 1 >= argv.size()) {
                errorOut = "--" + tok + " needs a value";
                return a;
            }
            value = argv[++i];
        }
        a.opts[tok] = value;
    }
    return a;
}

// A log that keeps every line, so `--json` can hand the whole thing back. A
// scripted caller has nowhere else to look when something goes wrong.
class CollectingLog : public actions::Log {
public:
    void Write(const std::string& msg, const std::string& level) override {
        lines.push_back("[" + level + "] " + msg);
    }
    std::vector<std::string> lines;
};

int Emit(const json& result, bool asJson, bool ok) {
    if (asJson) {
        std::cout << result.dump(2) << std::endl;
    } else {
        const auto err = result.find("error");
        if (err != result.end() && err->is_string()) {
            std::cerr << err->get<std::string>() << std::endl;
        }
        const auto steps = result.find("steps");
        if (steps != result.end() && steps->is_array()) {
            for (const json& s : *steps) {
                if (s.is_string()) std::cout << "  " << s.get<std::string>() << "\n";
            }
        }
        const auto aircraft = result.find("aircraft");
        if (aircraft != result.end() && aircraft->is_array()) {
            for (const json& ac : *aircraft) {
                std::cout << "  " << ac.value("airframe", "?") << "  "
                          << ac.value("folder", "?") << "  photon="
                          << (ac.value("photon", std::string()).empty()
                                  ? "-"
                                  : ac.value("photon", std::string()))
                          << (ac.value("stale", false) ? " (stale)" : "") << "\n";
            }
        }
        std::cout.flush();
    }
    return ok ? 0 : 1;
}

json AircraftToJson(const detect::Aircraft& ac) {
    json j = json::object();
    j["folder"] = ac.folder;
    j["path"] = ac.path;
    j["airframe"] = ac.airframe;
    j["ac_ver"] = ac.acVer;
    j["name"] = ac.name;
    j["photon"] = ac.photon;
    j["wing"] = ac.wing;
    j["stale"] = ac.stale;
    j["wings_offered"] = json(WingsFor(ac.airframe));
    j["has_interior"] = AirframeHasInterior(ac.airframe);
    j["has_screens"] = AirframeHasScreens(ac.airframe);
    return j;
}

json ManifestSummary(const manifest::Manifest& m) {
    json j = json::object();
    j["present"] = m.present;
    if (!m.present) return j;
    j["version"] = m.version;
    j["wing"] = m.wing;
    j["installed_at"] = m.installedAt;
    j["backed_up"] = json(m.backedUp);
    j["realwings_patched"] = m.realwingsPatched;
    j["interior_installed"] = m.interior.installed;
    j["screens_installed"] = m.screens.installed;
    return j;
}

// The X-Plane root a subcommand should act on.
//
// ⚠ NEVER PROMPTS AND NEVER FALLS BACK TO A GUESS. `--xplane-root` wins; otherwise
// the aircraft path is walked upward for a valid root, because "install into the
// tree this aircraft is actually in" is the only inference that cannot be wrong.
std::string ResolveRoot(const Args& a, const std::string& aircraftPath,
                        std::string& errorOut) {
    const std::string explicitRoot = a.Get("xplane-root");
    if (!explicitRoot.empty()) {
        if (!detect::IsValidRoot(explicitRoot)) {
            errorOut = "not an X-Plane 12 folder (needs Aircraft/ and Resources/): " +
                       explicitRoot;
            return std::string();
        }
        return explicitRoot;
    }
    if (!aircraftPath.empty()) {
        namespace fs = std::filesystem;
        for (fs::path p = fsutil::PathFromUtf8(aircraftPath); !p.empty();
             p = p.parent_path()) {
            const std::string utf8 = fsutil::PathToUtf8(p);
            if (detect::IsValidRoot(utf8)) return utf8;
            if (p == p.parent_path()) break;
        }
    }
    errorOut = "could not work out the X-Plane folder — pass --xplane-root";
    return std::string();
}

// ⚠ The guard is a HARD REFUSAL in CLI mode, not a wait. A scripted run has nobody
// to close the sim. `PluginIsCurrent` still lets an OBJ-only reinstall through,
// which is the same rule the interactive screen uses.
bool RunningGuardBlocks(const Args& a, const std::string& root,
                        std::string& errorOut) {
    if (a.Has("force")) return false;
    if (!detect::XplaneRunning()) return false;
    if (actions::PluginIsCurrent(root)) return false;
    errorOut = "X-Plane appears to be running and the plugin needs updating — "
               "close it first, or pass --force";
    return true;
}

int CmdDetect(const Args& a) {
    const bool asJson = a.Has("json");
    json out = json::object();
    std::string root = a.Get("xplane-root");
    if (root.empty()) {
        const auto candidates = detect::XplaneCandidates();
        json cand = json::array();
        for (const auto& c : candidates) {
            cand.push_back({{"path", c.path}, {"why", c.why}});
        }
        out["candidates"] = cand;
        if (!candidates.empty()) root = candidates.front().path;
    }
    if (root.empty() || !detect::IsValidRoot(root)) {
        out["ok"] = false;
        out["error"] = root.empty()
                           ? "no X-Plane 12 install found — pass --xplane-root"
                           : "not an X-Plane 12 folder: " + root;
        out["aircraft"] = json::array();
        return Emit(out, asJson, false);
    }
    out["xplane_root"] = root;
    json list = json::array();
    for (const detect::Aircraft& ac : detect::DetectAircraft(root)) {
        list.push_back(AircraftToJson(ac));
    }
    out["aircraft"] = list;
    out["ok"] = true;
    return Emit(out, asJson, true);
}

int CmdStatus(const Args& a) {
    const bool asJson = a.Has("json");
    json out = json::object();
    const std::string aircraft = a.Get("aircraft");
    if (aircraft.empty()) {
        out["ok"] = false;
        out["error"] = "--aircraft is required";
        return Emit(out, asJson, false);
    }
    const std::string objects =
        fsutil::PathToUtf8(fsutil::PathFromUtf8(aircraft) / "objects");
    const std::string airframe = detect::IdentifyAirframe(aircraft);
    const detect::Status ext = detect::PhotonStatus(objects);
    const detect::Status inter = detect::InteriorStatus(objects);

    out["aircraft"] = aircraft;
    out["airframe"] = airframe;
    out["exterior"] = {{"installed", ext.installed},
                       {"version", ext.version},
                       {"wing", ext.wing},
                       {"stale", ext.stale}};
    // ⚠ A SECOND, INDEPENDENT AXIS. Folding it into the exterior status would make
    // an aircraft with the interior installed and the exterior reverted read as
    // "not installed at all", hiding a live modification from the user.
    out["interior"] = {{"installed", inter.installed},
                       {"version", inter.version},
                       {"stale", inter.stale}};
    out["manifest"] = ManifestSummary(manifest::Read(objects));
    out["ok"] = true;
    return Emit(out, asJson, true);
}

int CmdInstall(const Args& a) {
    const bool asJson = a.Has("json");
    json out = json::object();
    CollectingLog log;

    actions::Options opts;
    opts.aircraftPath = a.Get("aircraft");
    opts.wing = a.Get("wing", "stock");
    opts.interior = a.Has("interior");
    opts.dryRun = a.Has("dry-run");

    if (opts.aircraftPath.empty()) {
        out["ok"] = false;
        out["error"] = "--aircraft is required";
        return Emit(out, asJson, false);
    }
    opts.airframeKey = detect::IdentifyAirframe(opts.aircraftPath);
    if (opts.airframeKey.empty()) {
        out["ok"] = false;
        out["error"] = "not a supported ToLiss aircraft: " + opts.aircraftPath;
        return Emit(out, asJson, false);
    }
    // ⚠ Checked HERE as well as in payload::ObjText, so the CLI's error names the
    // real reason (this airframe has no such variant) rather than a missing file.
    if (!WingIsOfferedFor(opts.airframeKey, opts.wing)) {
        out["ok"] = false;
        out["error"] = opts.airframeKey + " has no '" + opts.wing +
                       "' wing-mod variant";
        return Emit(out, asJson, false);
    }
    std::string err;
    opts.xplaneRoot = ResolveRoot(a, opts.aircraftPath, err);
    if (opts.xplaneRoot.empty()) {
        out["ok"] = false;
        out["error"] = err;
        return Emit(out, asJson, false);
    }
    if (RunningGuardBlocks(a, opts.xplaneRoot, err)) {
        out["ok"] = false;
        out["error"] = err;
        return Emit(out, asJson, false);
    }

    out["aircraft"] = opts.aircraftPath;
    out["airframe"] = opts.airframeKey;
    out["wing"] = opts.wing;
    out["xplane_root"] = opts.xplaneRoot;
    out["dry_run"] = opts.dryRun;
    try {
        out["steps"] = json(actions::Install(opts, log));
        out["ok"] = true;
    } catch (const std::exception& e) {
        out["ok"] = false;
        out["error"] = e.what();
    }
    out["log"] = json(log.lines);
    if (!opts.dryRun) {
        out["manifest"] = ManifestSummary(manifest::Read(
            fsutil::PathToUtf8(fsutil::PathFromUtf8(opts.aircraftPath) / "objects")));
    }
    return Emit(out, asJson, out.value("ok", false));
}

int CmdUninstall(const Args& a) {
    const bool asJson = a.Has("json");
    json out = json::object();
    CollectingLog log;

    actions::Options opts;
    opts.aircraftPath = a.Get("aircraft");
    opts.dryRun = a.Has("dry-run");
    opts.removePlugin = a.Has("remove-plugin");
    if (opts.aircraftPath.empty()) {
        out["ok"] = false;
        out["error"] = "--aircraft is required";
        return Emit(out, asJson, false);
    }
    opts.airframeKey = detect::IdentifyAirframe(opts.aircraftPath);
    std::string err;
    opts.xplaneRoot = ResolveRoot(a, opts.aircraftPath, err);
    if (opts.xplaneRoot.empty()) {
        out["ok"] = false;
        out["error"] = err;
        return Emit(out, asJson, false);
    }
    if (RunningGuardBlocks(a, opts.xplaneRoot, err)) {
        out["ok"] = false;
        out["error"] = err;
        return Emit(out, asJson, false);
    }

    out["aircraft"] = opts.aircraftPath;
    out["dry_run"] = opts.dryRun;
    try {
        out["steps"] = json(actions::Uninstall(opts, log));
        out["ok"] = true;
    } catch (const std::exception& e) {
        out["ok"] = false;
        out["error"] = e.what();
    }
    out["log"] = json(log.lines);
    return Emit(out, asJson, out.value("ok", false));
}

int CmdVersion(const Args& a) {
    if (!a.Has("json")) {
        std::cout << kPhotonVersion << std::endl;
        return 0;
    }
    json out = json::object();
    out["ok"] = true;
    out["version"] = kPhotonVersion;
    out["payload_dir"] = payload::PayloadDir();
    out["payload_available"] = payload::Available();
    out["realwings_available"] = payload::RealWingsAvailable();
    out["interior_available"] = payload::InteriorAvailable();
    out["screens_available"] = payload::ScreensAvailable();
    std::vector<std::string> arches;
    try {
        arches = payload::PluginArches();
    } catch (const payload::PayloadError&) {
    }
    out["plugin_arches"] = json(arches);
    return Emit(out, true, true);
}

}  // namespace

std::string UsageText() {
    return
        "ToLiss Photon Lighting installer\n"
        "\n"
        "  photon-installer                        the interactive installer\n"
        "  photon-installer detect  [--xplane-root P] [--json]\n"
        "  photon-installer status  --aircraft P [--json]\n"
        "  photon-installer install --aircraft P [--wing stock|durantula|realwings]\n"
        "                           [--interior] [--dry-run] [--xplane-root P]\n"
        "                           [--force] [--json]\n"
        "  photon-installer uninstall --aircraft P [--remove-plugin] [--dry-run]\n"
        "                           [--xplane-root P] [--force] [--json]\n"
        "  photon-installer version [--json]\n"
        "\n"
        "--json prints a machine-readable result on stdout; the exit code is 0 or 1.\n"
        "There are no prompts: missing required input is an error, and a running\n"
        "X-Plane is a refusal unless --force.\n";
}

bool IsCliInvocation(const std::vector<std::string>& args) {
    if (args.size() < 2) return false;
    const std::string first = args[1];
    if (fsutil::StartsWith(first, "-")) {
        // `--help`/`--version` are CLI; anything else the TUI understands is its own.
        return first == "--help" || first == "-h" || first == "--version";
    }
    return Subcommands().count(first) > 0;
}

int RunCli(const std::vector<std::string>& argv) {
    if (argv.size() >= 2 && (argv[1] == "--help" || argv[1] == "-h")) {
        std::cout << UsageText();
        return 0;
    }
    if (argv.size() >= 2 && argv[1] == "--version") {
        std::cout << kPhotonVersion << std::endl;
        return 0;
    }

    std::string parseError;
    const Args a = ParseArgs(argv, parseError);
    if (!parseError.empty()) {
        std::cerr << parseError << "\n\n" << UsageText();
        return 2;
    }
    if (a.command.empty() || a.command == "help" || a.Has("help")) {
        std::cout << UsageText();
        return 0;
    }
    if (a.command == "detect") return CmdDetect(a);
    if (a.command == "status") return CmdStatus(a);
    if (a.command == "install") return CmdInstall(a);
    if (a.command == "uninstall") return CmdUninstall(a);
    if (a.command == "version") return CmdVersion(a);

    std::cerr << "unknown subcommand: " << a.command << "\n\n" << UsageText();
    return 2;
}

}  // namespace installer
}  // namespace photon
