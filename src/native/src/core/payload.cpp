#include "core/payload.h"

#include <algorithm>
#include <sstream>

#include "core/constants.h"
#include "core/fsutil.h"

namespace photon {
namespace payload {
namespace {

namespace fs = std::filesystem;

std::string gPayloadDir;

std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::ostringstream os;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) os << sep;
        os << v[i];
    }
    return os.str();
}

fs::path Dir() { return fsutil::PathFromUtf8(gPayloadDir); }

// The one place that explains a missing bundle, INCLUDING the dev recipe — since
// the fix for a repo checkout is no longer "it builds itself".
void RequireBundle() {
    if (Available()) return;
    throw PayloadError(
        "no bundled payload at " + gPayloadDir +
        " — this build is incomplete. From a repo checkout, stage one first: "
        "python build/make_release.py --payload-only");
}

std::string ReadTextOrThrow(const fs::path& p, const char* what) {
    std::string text;
    if (!fsutil::ReadFileBytes(p, text)) {
        throw PayloadError(std::string("missing bundled ") + what + ": " +
                           fsutil::PathToUtf8(p));
    }
    return text;
}

}  // namespace

void InitFromExecutable(const std::string& exePathUtf8) {
    std::error_code ec;
    fs::path exe = fs::weakly_canonical(fsutil::PathFromUtf8(exePathUtf8), ec);
    if (ec) exe = fsutil::PathFromUtf8(exePathUtf8);
    const fs::path dir = exe.parent_path();

    // ⚠ TWO NAMES, ONE BINARY. `payload/` is what `make_release.py` stages in a
    // checkout and the canonical name. `data/` is what the shipped per-platform
    // bundle calls it — the plainer end-user name, documented in its README — and
    // this same executable has to run out of both: a dev restages `payload/` and
    // reruns, a user downloads `data/`. Renaming either would break the other for
    // no gain; a second accepted name is one line.
    //
    // Order matters: `payload/` wins, so a repo checkout that has staged one is
    // never shadowed by a stale `data/` left over from a bundle build.
    gPayloadDir = fsutil::PathToUtf8(dir / "payload");
    if (!Available()) {
        const std::string alt = fsutil::PathToUtf8(dir / "data");
        const std::string canonical = gPayloadDir;
        gPayloadDir = alt;
        if (!Available()) gPayloadDir = canonical;   // report the canonical name
    }
}

void SetPayloadDir(const std::string& dirUtf8) { gPayloadDir = dirUtf8; }

std::string PayloadDir() { return gPayloadDir; }

bool Available() {
    std::error_code ec;
    return !gPayloadDir.empty() && fs::is_directory(Dir(), ec) && !ec;
}

std::string ObjPath(const std::string& wing, const std::string& airframeKey) {
    const Airframe* af = AirframeByKey(airframeKey);
    if (af == nullptr) throw PayloadError("unknown airframe: " + airframeKey);
    const fs::path p = Dir() / "objs" / fsutil::PathFromUtf8(wing) / af->objName;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) {
        throw PayloadError("missing bundled OBJ: " + fsutil::PathToUtf8(p));
    }
    return fsutil::PathToUtf8(p);
}

std::string ObjText(const std::string& wing, const std::string& airframeKey) {
    // ⚠ BEFORE the bundle check — see the header. "a339 has no 'durantula'
    // variant" is the true reason; "this build is incomplete" is a wrong one that
    // sends the user to re-download.
    if (!WingIsOfferedFor(airframeKey, wing)) {
        throw PayloadError(airframeKey + " has no '" + wing +
                           "' wing-mod variant (supported: " +
                           Join(WingsFor(airframeKey), ", ") + ")");
    }
    RequireBundle();
    return ReadTextOrThrow(fsutil::PathFromUtf8(ObjPath(wing, airframeKey)), "OBJ");
}

std::string InteriorObjPath() {
    const fs::path p = Dir() / "objs" / "interior" / kInteriorObj;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) {
        throw PayloadError("missing bundled interior OBJ: " + fsutil::PathToUtf8(p));
    }
    return fsutil::PathToUtf8(p);
}

std::string InteriorObjText() {
    RequireBundle();
    return ReadTextOrThrow(fsutil::PathFromUtf8(InteriorObjPath()), "interior OBJ");
}

std::string ScreensObjPath(const std::string& airframeKey) {
    // ⚠ The airframe gate fires BEFORE the file check, the same way ObjPath's wing
    // gate does and for the same reason: an airframe this feature does not cover
    // must report "not supported here", never "your download is incomplete".
    if (!AirframeHasScreens(airframeKey)) {
        throw PayloadError("the display-glow OBJ is not supported on " +
                           airframeKey);
    }
    const fs::path p = Dir() / "objs" / "screens" / airframeKey / kScreensObj;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) {
        throw PayloadError("missing bundled screen-glow OBJ: " +
                           fsutil::PathToUtf8(p));
    }
    return fsutil::PathToUtf8(p);
}

std::string ScreensObjText(const std::string& airframeKey) {
    RequireBundle();
    return ReadTextOrThrow(fsutil::PathFromUtf8(ScreensObjPath(airframeKey)),
                           "screen-glow OBJ");
}

bool ScreensAvailable(const std::string& airframeKey) {
    try {
        (void)ScreensObjPath(airframeKey);
        return Available();
    } catch (const PayloadError&) {
        return false;
    }
}

std::vector<std::pair<std::string, std::string>> InteriorTextureFiles() {
    const fs::path src = Dir() / "textures" / "interior";
    std::error_code ec;
    if (!fs::is_directory(src, ec) || ec) {
        throw PayloadError("interior texture set not found: " +
                           fsutil::PathToUtf8(src) +
                           " — this build is incomplete");
    }
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::string> missing;
    for (const std::string& name : InteriorTextures()) {
        const fs::path p = src / fsutil::PathFromUtf8(name);
        std::error_code fe;
        if (fs::is_regular_file(p, fe) && !fe) {
            out.emplace_back(name, fsutil::PathToUtf8(p));
        } else {
            missing.push_back(name);
        }
    }
    if (!missing.empty()) {
        // ⚠ An error, not a silent skip: shipping 9 of 11 leaves the cockpit
        // half-retextured, which reads in-sim as the mod being broken.
        throw PayloadError("interior texture set is incomplete in " +
                           fsutil::PathToUtf8(src) + " — missing: " +
                           Join(missing, ", "));
    }
    return out;
}

bool InteriorAvailable() {
    try {
        (void)InteriorObjPath();
        (void)InteriorTextureFiles();
        return Available();
    } catch (const PayloadError&) {
        return false;
    }
}

std::string RealWingsPatchPath() {
    return fsutil::PathToUtf8(Dir() / patch_realwings::kPatchJson);
}

patch_realwings::PatchData RealWingsPatchData() {
    return patch_realwings::Load(RealWingsPatchPath());
}

bool RealWingsAvailable() {
    try {
        (void)RealWingsPatchData();
        return true;
    } catch (const patch_realwings::RealWingsError&) {
        return false;
    }
}

std::string PluginSrcRoot() {
    const fs::path p = Dir() / "plugin" / kPluginFolder;
    std::error_code ec;
    if (!fs::is_directory(p, ec) || ec) {
        throw PayloadError("native plugin folder not found: " +
                           fsutil::PathToUtf8(p) +
                           " — this release bundle is incomplete");
    }
    return fsutil::PathToUtf8(p);
}

std::vector<std::pair<std::string, std::string>> PluginFiles() {
    const fs::path root = fsutil::PathFromUtf8(PluginSrcRoot());
    std::vector<std::pair<std::string, std::string>> out;
    for (const fsutil::FileEntry& e : fsutil::ListFilesRecursive(root)) {
        // ⚠ `overlays/` is the user's territory. A source folder that happens to be
        // a DEPLOYED plugin folder holds that dev's own images.
        if (PluginPathIsUserOwned(e.rel)) continue;
        out.emplace_back(e.rel, fsutil::PathToUtf8(e.abs));
    }
    if (out.empty()) {
        throw PayloadError("native plugin folder is empty (no .xpl built?): " +
                           fsutil::PathToUtf8(root));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> PluginDataFiles() {
    std::vector<std::pair<std::string, std::string>> out;
    const fs::path root = Dir() / kPluginDataDirName;
    std::error_code ec;
    // None, not an error: a bundle built before the compositor shipped installs
    // the plugin and nothing else, rather than refusing to run.
    if (!fs::is_directory(root, ec) || ec) return out;

    const fs::path fx = root / kPanelFxFile;
    std::error_code fe;
    if (fs::is_regular_file(fx, fe) && !fe) {
        out.emplace_back(kPanelFxFile, fsutil::PathToUtf8(fx));
    }
    for (const std::string& d : PluginUserDirs()) {
        const fs::path sub = root / fsutil::PathFromUtf8(d);
        std::error_code de;
        if (!fs::is_directory(sub, de) || de) continue;
        for (const fsutil::FileEntry& e : fsutil::ListFilesRecursive(sub)) {
            out.emplace_back(d + "/" + e.rel, fsutil::PathToUtf8(e.abs));
        }
    }
    return out;
}

std::vector<std::string> PluginArches() {
    const fs::path root = fsutil::PathFromUtf8(PluginSrcRoot());
    std::vector<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code de;
        if (!it->is_directory(de) || de) continue;
        std::error_code fe;
        if (fs::is_regular_file(it->path() / kXplName, fe) && !fe) {
            out.push_back(fsutil::PathToUtf8(it->path().filename()));
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace payload
}  // namespace photon
