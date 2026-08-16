#include "core/detect.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <set>

#include "core/constants.h"
#include "core/fsutil.h"
#include "core/manifest.h"
#include "core/marker.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cstdio>
#endif

namespace photon {
namespace detect {
namespace {

namespace fs = std::filesystem;

constexpr char kSkunkCfgName[] = "skunkcrafts_updater.cfg";
// The marker sits within the first few lines of an OBJ by construction, so the
// scan never reads more than this — these files run to tens of KB.
constexpr std::size_t kMarkerScanBytes = 2000;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string HomeDir() {
#ifdef _WIN32
    const std::string p = Env("USERPROFILE");
    if (!p.empty()) return p;
    return Env("HOMEDRIVE") + Env("HOMEPATH");
#else
    return Env("HOME");
#endif
}

std::string ReadHead(const fs::path& p, std::size_t maxBytes) {
    std::string text;
    if (!fsutil::ReadFileBytes(p, text)) return std::string();
    if (text.size() > maxBytes) text.resize(maxBytes);
    return text;
}

std::string UpperCopy(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// Does any `.acf` in the aircraft folder stamp `P acf/_ICAO <want>`? The
// corroboration behind `Airframe::acfIcao` — the file's own model identity,
// the same content-over-naming rule as the format stamp `acf::FormatVersion`
// reads. Any variant may answer: all four of a ToLiss folder's `.acf` files
// carry the same ICAO, and this is identification, not patching, so the
// XP11-pair rule does not apply.
//
// STREAMED, not ReadFileBytes: the property block is sorted, so `acf/_ICAO`
// sits tens of thousands of lines into a 20-50 MB file, and whole-file reads
// of all four variants would be most of a detection scan's I/O for one field.
// Stops at the stamp or at PROPERTIES_END.
//
// ⚠ `.acf` EXACTLY — both wing mods leave `*.acf.bak` and
// `*.acf.durantula.bak` copies behind forever, and a stale copy's ICAO is
// exactly what this must not answer from. (`EndsWith(".acf")` excludes them.)
bool FolderHasAcfIcao(const std::string& folderUtf8, const char* want) {
    constexpr char kIcaoProp[] = "P acf/_ICAO ";
    const std::string wantUpper = UpperCopy(want);
    const fs::path folder = fsutil::PathFromUtf8(folderUtf8);
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        const fs::path& p = it->path();
        const std::string name = fsutil::ToLower(fsutil::PathToUtf8(p.filename()));
        if (!fsutil::EndsWith(name, ".acf")) continue;
        std::error_code fec;
        if (!fs::is_regular_file(p, fec) || fec) continue;
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (fsutil::StartsWith(line, kIcaoProp)) {
                const std::string got =
                    UpperCopy(fsutil::Trim(line.substr(sizeof(kIcaoProp) - 1)));
                if (got == wantUpper) return true;
                break;   // one stamp per file; try the next variant
            }
            if (line == "PROPERTIES_END") break;
        }
    }
    return false;
}

}  // namespace

bool IsValidRoot(const std::string& pathUtf8) {
    if (pathUtf8.empty()) return false;
    const fs::path p = fsutil::PathFromUtf8(pathUtf8);
    std::error_code ec;
    return fs::is_directory(p, ec) && !ec &&
           fs::is_directory(p / "Aircraft", ec) && !ec &&
           fs::is_directory(p / "Resources", ec) && !ec;
}

std::vector<std::string> ParseLibraryVdf(const std::string& text) {
    std::vector<std::string> out;
    for (const std::string& rawLine : fsutil::SplitLines(text)) {
        const std::string line = fsutil::Trim(rawLine);
        if (!fsutil::StartsWith(line, "\"path\"")) continue;
        // `"path"   "D:\\SteamLibrary"` — the fourth quote-delimited field.
        std::vector<std::string> parts;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == '"') {
                parts.push_back(line.substr(start, i - start));
                start = i + 1;
            }
        }
        if (parts.size() < 4) continue;
        std::string p = parts[3];
        // VDF escapes backslashes; the path we want is the unescaped one.
        std::string unescaped;
        for (std::size_t i = 0; i < p.size(); ++i) {
            unescaped.push_back(p[i]);
            if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
        }
        out.push_back(unescaped);
    }
    return out;
}

namespace {

std::vector<fs::path> SteamLibraryRoots() {
    std::vector<fs::path> steamCandidates;
#ifdef _WIN32
    for (const char* env : {"ProgramFiles(x86)", "ProgramFiles"}) {
        const std::string base = Env(env);
        if (!base.empty()) {
            steamCandidates.push_back(fsutil::PathFromUtf8(base) / "Steam");
        }
    }
#elif defined(__APPLE__)
    steamCandidates.push_back(fsutil::PathFromUtf8(HomeDir()) / "Library" /
                              "Application Support" / "Steam");
#else
    steamCandidates.push_back(fsutil::PathFromUtf8(HomeDir()) / ".steam" / "steam");
    steamCandidates.push_back(fsutil::PathFromUtf8(HomeDir()) / ".local" / "share" /
                              "Steam");
#endif

    std::vector<fs::path> roots;
    for (const fs::path& steam : steamCandidates) {
        roots.push_back(steam);
        const fs::path vdf = steam / "steamapps" / "libraryfolders.vdf";
        std::string text;
        if (fsutil::ReadFileBytes(vdf, text)) {
            for (const std::string& p : ParseLibraryVdf(text)) {
                roots.push_back(fsutil::PathFromUtf8(p));
            }
        }
    }
    return roots;
}

std::vector<fs::path> CommonDirs() {
    std::vector<fs::path> dirs;
#ifdef _WIN32
    for (const char* env : {"ProgramFiles", "ProgramFiles(x86)"}) {
        const std::string base = Env(env);
        if (!base.empty()) dirs.push_back(fsutil::PathFromUtf8(base));
    }
    for (char letter = 'A'; letter <= 'Z'; ++letter) {
        const fs::path root = fs::path(std::string(1, letter) + ":\\");
        std::error_code ec;
        if (fs::is_directory(root, ec) && !ec) dirs.push_back(root);
    }
#elif defined(__APPLE__)
    dirs.push_back("/Applications");
    dirs.push_back(fsutil::PathFromUtf8(HomeDir()));
#else
    dirs.push_back(fsutil::PathFromUtf8(HomeDir()));
    dirs.push_back("/opt");
    dirs.push_back("/usr/local");
#endif
    return dirs;
}

}  // namespace

std::vector<RootCandidate> XplaneCandidates(const std::string& selfPathUtf8) {
    std::vector<RootCandidate> found;
    std::set<std::string> seen;

    const auto consider = [&](const fs::path& p, const char* why) {
        if (p.empty()) return;
        std::error_code ec;
        fs::path resolved = fs::weakly_canonical(p, ec);
        if (ec) resolved = p;
        const std::string utf8 = fsutil::PathToUtf8(resolved);
        if (!IsValidRoot(utf8)) return;
        if (!seen.insert(fsutil::ToLower(utf8)).second) return;
        found.push_back({utf8, why});
    };

    const std::string env = Env("XPLANE_ROOT");
    if (!env.empty()) consider(fsutil::PathFromUtf8(env), "$XPLANE_ROOT");

    // The installer may be run from INSIDE an existing X-Plane install (e.g. from
    // Resources/plugins/). Walk our own path's parents looking for a root.
    if (!selfPathUtf8.empty()) {
        std::error_code ec;
        fs::path here = fs::weakly_canonical(fsutil::PathFromUtf8(selfPathUtf8), ec);
        if (ec) here = fsutil::PathFromUtf8(selfPathUtf8);
        for (fs::path p = here; !p.empty(); p = p.parent_path()) {
            if (IsValidRoot(fsutil::PathToUtf8(p))) {
                consider(p, "running from inside X-Plane");
                break;
            }
            if (p == p.parent_path()) break;
        }
    }

    for (const fs::path& steam : SteamLibraryRoots()) {
        consider(steam / "steamapps" / "common" / "X-Plane 12", "steam library");
    }
    for (const fs::path& base : CommonDirs()) {
        for (const char* name : {"X-Plane 12", "X-Plane12", "X-Plane 12 Steam"}) {
            consider(base / name, "common install dir");
        }
    }
    return found;
}

bool IsXplaneProcessName(const std::string& raw) {
    // Trim: `ps` pads and always leaves the newline on.
    std::size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return false;
    const std::size_t e = raw.find_last_not_of(" \t\r\n");
    std::string name = raw.substr(b, e - b + 1);

    // Basename. macOS reports the whole path here; the other two do not.
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);

    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });

    // ⚠ STRIP `.exe` BEFORE COMPARING, so one rule covers all three platforms.
    const std::string kExe = ".exe";
    if (name.size() > kExe.size() &&
        name.compare(name.size() - kExe.size(), kExe.size(), kExe) == 0) {
        name.resize(name.size() - kExe.size());
    }

    // ⚠ EXACT, OR THE `x-plane-` PREFIX — never a bare `x-plane` prefix. The
    // hyphen is what admits Linux's `X-Plane-x86_64` while still rejecting
    // `X-Plane 12 Installer`, whose basename starts `x-plane ` with a SPACE.
    return name == "x-plane" || name.rfind("x-plane-", 0) == 0;
}

bool XplaneRunning() {
    // ⚠ ANY FAILURE MEANS "NOT RUNNING". This check exists to warn the user, so it
    // must never be able to block an install by failing to introspect processes.
#ifdef _WIN32
    // ⚠ In-process, not `tasklist`: this runs on an interactive screen AND in its
    // 2-second poll loop, so spawning a process per tick is the wrong shape.
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (::Process32FirstW(snap, &entry)) {
        do {
            // Narrowed rather than compared as wide: the image name is ASCII, and
            // routing both platforms through ONE matcher is the point — a rule that
            // lived twice is how the two drifted apart in the first place.
            const std::wstring wide(entry.szExeFile);
            std::string name;
            name.reserve(wide.size());
            for (const wchar_t c : wide) {
                name.push_back(c < 128 ? static_cast<char>(c) : '?');
            }
            if (IsXplaneProcessName(name)) {
                running = true;
                break;
            }
        } while (::Process32NextW(snap, &entry));
    }
    ::CloseHandle(snap);
    return running;
#else
    FILE* pipe = ::popen("ps -A -o comm=", "r");
    if (pipe == nullptr) return false;
    char buf[512];
    bool running = false;
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        if (IsXplaneProcessName(buf)) {
            running = true;
            break;
        }
    }
    ::pclose(pipe);
    return running;
#endif
}

SkunkCfg ParseSkunkCfg(const std::string& text) {
    // A pipe-delimited `key|value` file: `version|v1.12`, `name|ToLiss A319`.
    // Key ORDER VARIES, so the whole file is parsed rather than probed.
    SkunkCfg cfg;
    for (const std::string& line : fsutil::SplitLines(text)) {
        const std::size_t bar = line.find('|');
        if (bar == std::string::npos) continue;
        const std::string key = fsutil::Trim(line.substr(0, bar));
        const std::string value = fsutil::Trim(line.substr(bar + 1));
        if (key == "module") cfg.module = value;
        else if (key == "name") cfg.name = value;
        else if (key == "version") cfg.version = value;
    }
    while (!cfg.version.empty() && (cfg.version.front() == 'v' ||
                                    cfg.version.front() == 'V')) {
        cfg.version.erase(cfg.version.begin());
    }
    cfg.version = fsutil::Trim(cfg.version);
    return cfg;
}

SkunkCfg ReadSkunkCfg(const std::string& folderUtf8) {
    const fs::path folder = fsutil::PathFromUtf8(folderUtf8);
    fs::path path = folder / kSkunkCfgName;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        // Case-insensitive fallback: the file is named by ToLiss's tooling and has
        // been seen with different casing.
        path.clear();
        for (fs::directory_iterator it(folder, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            const std::string name =
                fsutil::ToLower(fsutil::PathToUtf8(it->path().filename()));
            if (name == kSkunkCfgName) {
                path = it->path();
                break;
            }
        }
        if (path.empty()) return SkunkCfg();
    }
    std::string text;
    if (!fsutil::ReadFileBytes(path, text)) return SkunkCfg();
    return ParseSkunkCfg(text);
}

std::string IdentifyAirframe(const std::string& folderUtf8) {
    const SkunkCfg cfg = ReadSkunkCfg(folderUtf8);
    const std::string module = UpperCopy(cfg.module);
    const std::string name = UpperCopy(cfg.name);

    // 1. the update-repo URL — the most stable machine id.
    for (const Airframe& af : Airframes()) {
        if (!module.empty() &&
            module.find(UpperCopy(af.cfgModule)) != std::string::npos) {
            return af.key;
        }
    }
    // 2. the display name.
    for (const Airframe& af : Airframes()) {
        if (!name.empty() &&
            name.find(UpperCopy(af.cfgName)) != std::string::npos) {
            return af.key;
        }
    }
    // 3. the airframe-specific OBJ filename — intrinsic to the aircraft's files.
    //
    // ⚠ A GENERIC fingerprint needs the `.acf`'s own word for it. The a339's
    // `ExternalLights_XP12.obj` is ToLiss's name for every newer airframe's
    // exterior lights OBJ — an A340 carries it too, and presence alone
    // misdetected an A340 as an installable A330-900 (2026-08-16). A failed
    // corroboration falls through rather than returning empty here: a real
    // A339 whose `.acf` files are unreadable still identifies by step 4's
    // folder glob, while an A340 matches nothing and stays off the list.
    const fs::path objects = fsutil::PathFromUtf8(folderUtf8) / "objects";
    for (const Airframe& af : Airframes()) {
        std::error_code ec;
        if (!fs::is_regular_file(objects / af.objName, ec) || ec) continue;
        if (af.acfIcao[0] != '\0' && !FolderHasAcfIcao(folderUtf8, af.acfIcao))
            continue;
        return af.key;
    }
    // 4. the legacy folder-name glob, last resort.
    const std::string folderName =
        fsutil::PathToUtf8(fsutil::PathFromUtf8(folderUtf8).filename());
    for (const Airframe& af : Airframes()) {
        if (fsutil::GlobMatch(af.folderGlob, folderName)) return af.key;
    }
    return std::string();
}

namespace {

// The `# ToLissPhoton …` marker out of whichever `lights_out` OBJ lives in this
// objects/ folder — only one airframe's file is ever present.
marker::Marker ExteriorMarker(const fs::path& objects) {
    for (const Airframe& af : Airframes()) {
        const fs::path p = objects / af.objName;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec) || ec) continue;
        const marker::Marker m = marker::Parse(ReadHead(p, kMarkerScanBytes));
        if (m.found) return m;
    }
    return marker::Marker();
}

}  // namespace

Status PhotonStatus(const std::string& objectsDirUtf8) {
    Status out;
    const fs::path objects = fsutil::PathFromUtf8(objectsDirUtf8);
    const manifest::Manifest m = manifest::Read(objectsDirUtf8);
    const marker::Marker obj = ExteriorMarker(objects);

    if (m.present && !m.version.empty()) {
        // ⚠ The manifest is authoritative for version and wing, but the MARKER
        // decides whether the edit is actually still in place. Marker gone => an
        // aircraft update reverted the OBJ while leaving the bookkeeping intact.
        out.version = m.version;
        out.wing = m.wing;
        out.installed = true;
        out.stale = !obj.found;
        return out;
    }
    if (obj.found) {
        // A manifest-less hand-copy, or a pre-manifest install.
        out.version = obj.version;
        out.wing = obj.wing;
        out.installed = true;
    }
    return out;
}

Status InteriorStatus(const std::string& objectsDirUtf8) {
    Status out;
    const fs::path objects = fsutil::PathFromUtf8(objectsDirUtf8);
    const manifest::Manifest m = manifest::Read(objectsDirUtf8);

    marker::Marker objMarker;
    const fs::path p = objects / kInteriorObj;
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && !ec) {
        objMarker = marker::Parse(ReadHead(p, kMarkerScanBytes));
    }

    if (m.interior.installed) {
        out.version = m.interior.version;
        out.installed = true;
        out.stale = !objMarker.found;
        return out;
    }
    if (objMarker.found) {
        out.version = objMarker.version;
        out.installed = true;    // manifest-less hand-copy
    }
    return out;
}

namespace {

// Every folder under Aircraft/, at ANY depth, that could be a ToLiss aircraft.
//
// A folder qualifies if it holds a SkunkCrafts cfg (the primary, name-agnostic
// signal), an `.acf` (X-Plane's canonical "this is an aircraft" marker, so even a
// cfg-less oddly-named copy is reached), or its name matches a legacy glob.
//
// ⚠ ONCE A FOLDER QUALIFIES, ITS SUBTREE IS PRUNED: aircraft are not nested inside
// aircraft, so descending into liveries/objects/ is pure cost — a big win on
// installs with many liveries.
std::vector<fs::path> CandidateFolders(const fs::path& acDir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(acDir, ec)) return out;

    fs::recursive_directory_iterator it(acDir, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code de;
        if (!it->is_directory(de) || de) continue;
        const fs::path folder = it->path();

        bool hasCfg = false, hasAcf = false;
        std::error_code fe;
        for (fs::directory_iterator f(folder, fe), fend; f != fend;
             f.increment(fe)) {
            if (fe) break;
            std::error_code re;
            if (!f->is_regular_file(re) || re) continue;
            const std::string name =
                fsutil::ToLower(fsutil::PathToUtf8(f->path().filename()));
            if (name == kSkunkCfgName) hasCfg = true;
            if (fsutil::EndsWith(name, ".acf")) hasAcf = true;
            if (hasCfg && hasAcf) break;
        }
        bool nameHit = false;
        const std::string folderName = fsutil::PathToUtf8(folder.filename());
        for (const Airframe& af : Airframes()) {
            if (fsutil::GlobMatch(af.folderGlob, folderName)) {
                nameHit = true;
                break;
            }
        }
        if (hasCfg || hasAcf || nameHit) {
            out.push_back(folder);
            it.disable_recursion_pending();
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::vector<Aircraft> DetectAircraft(const std::string& xplaneRootUtf8) {
    std::vector<Aircraft> found;
    const fs::path root = fsutil::PathFromUtf8(xplaneRootUtf8);
    const fs::path acDir = root / "Aircraft";
    std::error_code ec;
    if (!fs::is_directory(acDir, ec)) return found;

    for (const fs::path& folder : CandidateFolders(acDir)) {
        const std::string folderUtf8 = fsutil::PathToUtf8(folder);
        const std::string airframe = IdentifyAirframe(folderUtf8);
        if (airframe.empty()) continue;    // not one of ours
        const Airframe* af = AirframeByKey(airframe);

        const fs::path objects = folder / "objects";
        Status st;
        std::error_code oe;
        if (fs::is_directory(objects, oe) && !oe) {
            st = PhotonStatus(fsutil::PathToUtf8(objects));
        }
        const SkunkCfg cfg = ReadSkunkCfg(folderUtf8);

        Aircraft ac;
        std::error_code re;
        const fs::path rel = fs::relative(folder, acDir, re);
        ac.folder = re ? fsutil::PathToUtf8(folder.filename())
                       : rel.generic_string();
        ac.path = folderUtf8;
        ac.airframe = airframe;
        ac.acVer = cfg.version.empty() ? "?" : cfg.version;
        ac.name = !cfg.name.empty() ? cfg.name
                                    : (af ? af->prettyName : airframe.c_str());
        ac.photon = st.version;
        ac.wing = st.wing;
        ac.stale = st.stale;
        found.push_back(std::move(ac));
    }
    return found;
}

bool AnyOtherAirframeHasPhoton(const std::string& xplaneRootUtf8,
                               const std::string& excludePathUtf8) {
    for (const Aircraft& ac : DetectAircraft(xplaneRootUtf8)) {
        if (ac.path != excludePathUtf8 && !ac.photon.empty()) return true;
    }
    return false;
}

}  // namespace detect
}  // namespace photon
