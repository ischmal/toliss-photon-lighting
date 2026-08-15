#include "core/manifest.h"

#include <algorithm>
#include <set>

#include "core/backup.h"
#include "core/constants.h"
#include "core/fsutil.h"
#include "third_party/nlohmann/json.hpp"

namespace photon {
namespace manifest {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// The keys this build understands. Anything else in the file is carried through
// untouched, so an older installer cannot silently drop a newer one's bookkeeping.
const char* const kKnownTopLevel[] = {
    "version", "wing", "installed_at", "backed_up", "added",
    "realwings_patched", "realwings_dir", "interior", "screens",
};

std::vector<std::string> StringArray(const json& j, const char* key) {
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const json& v : *it) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::string StringOr(const json& j, const char* key, const std::string& fallback) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

bool BoolOr(const json& j, const char* key, bool fallback) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0.0;
    return fallback;
}

void PutStrings(json& j, const char* key, const std::vector<std::string>& v) {
    j[key] = json(v);
}

}  // namespace

// ⚠ THE FOLDER IS RESOLVED, NEVER SPELLED OUT. It sits beside `objects/` on a
// current install and inside it on one an earlier installer wrote, and
// `backup::Dir` is the single statement of which — see core/backup.h. A manifest
// read from the wrong folder
// comes back ABSENT, and an install that believes nothing is installed backs up
// the file it wrote last time as though it were ToLiss's.
std::string PathFor(const std::string& objectsDirUtf8) {
    const fs::path p =
        backup::DirForObjects(fsutil::PathFromUtf8(objectsDirUtf8)) / kManifestName;
    return fsutil::PathToUtf8(p);
}

Manifest Parse(const std::string& text) {
    Manifest m;
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception&) {
        return m;                       // ⚠ corrupt reads as ABSENT, never as an error
    }
    if (!root.is_object()) return m;

    m.present = true;
    m.version = StringOr(root, "version", "");
    m.wing = StringOr(root, "wing", "");
    m.installedAt = StringOr(root, "installed_at", "");
    m.backedUp = StringArray(root, "backed_up");
    m.added = StringArray(root, "added");
    m.realwingsPatched = BoolOr(root, "realwings_patched", false);
    m.realwingsDir = StringOr(root, "realwings_dir", "");

    const auto interior = root.find("interior");
    if (interior != root.end() && interior->is_object()) {
        m.interior.installed = BoolOr(*interior, "installed", false);
        m.interior.version = StringOr(*interior, "version", "");
        m.interior.airframe = StringOr(*interior, "airframe", "");
        m.interior.added = StringArray(*interior, "added");
        m.interior.liveriesPatched = StringArray(*interior, "liveries_patched");
        m.interior.acfPatched = StringArray(*interior, "acf_patched");
    }

    const auto screens = root.find("screens");
    if (screens != root.end() && screens->is_object()) {
        m.screens.installed = BoolOr(*screens, "installed", false);
        m.screens.version = StringOr(*screens, "version", "");
        m.screens.added = StringArray(*screens, "added");
        m.screens.acfPatched = StringArray(*screens, "acf_patched");
    }

    // Preserve everything we did not model.
    json extra = json::object();
    const std::set<std::string> known(std::begin(kKnownTopLevel),
                                      std::end(kKnownTopLevel));
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (known.count(it.key()) == 0) extra[it.key()] = it.value();
    }
    if (!extra.empty()) m.unknownJson = extra.dump();
    return m;
}

Manifest Read(const std::string& objectsDirUtf8) {
    Manifest m;
    const fs::path p = fsutil::PathFromUtf8(PathFor(objectsDirUtf8));
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) return m;
    std::string text;
    if (!fsutil::ReadFileBytes(p, text)) return m;
    return Parse(text);
}

std::string Serialize(const Manifest& m) {
    json root = json::object();

    // Unknown keys go in FIRST so a modelled key of the same name still wins —
    // carrying a stranger's bookkeeping must never let it shadow ours.
    if (!m.unknownJson.empty()) {
        try {
            const json extra = json::parse(m.unknownJson);
            if (extra.is_object()) {
                for (auto it = extra.begin(); it != extra.end(); ++it) {
                    root[it.key()] = it.value();
                }
            }
        } catch (const json::exception&) {
            // Unparseable carry-through is simply dropped; it came from a file we
            // already decided was readable, so this cannot be the common case.
        }
    }

    root["version"] = m.version.empty() ? json(nullptr) : json(m.version);
    root["wing"] = m.wing.empty() ? json(nullptr) : json(m.wing);
    root["installed_at"] =
        m.installedAt.empty() ? json(nullptr) : json(m.installedAt);
    PutStrings(root, "backed_up", m.backedUp);

    // ⚠ Written ONLY when non-empty, on the `realwings_patched` precedent: the
    // case that fills it is rare, so an ordinary install's manifest stays byte
    // for byte what every prior version wrote.
    if (!m.added.empty()) PutStrings(root, "added", m.added);

    // ⚠ Written ONLY when true, matching the Python side: an install that never
    // touched RealWings leaves no key at all, so an older installer reading this
    // file sees exactly what it used to.
    if (m.realwingsPatched) {
        root["realwings_patched"] = true;
        root["realwings_dir"] = m.realwingsDir;
    }

    if (m.interior.installed || !m.interior.added.empty() ||
        !m.interior.liveriesPatched.empty() || !m.interior.acfPatched.empty()) {
        json j = json::object();
        PutStrings(j, "added", m.interior.added);
        PutStrings(j, "liveries_patched", m.interior.liveriesPatched);
        PutStrings(j, "acf_patched", m.interior.acfPatched);
        j["installed"] = m.interior.installed;
        j["version"] = m.interior.version;
        j["airframe"] = m.interior.airframe;
        root["interior"] = j;
    }

    if (m.screens.installed || !m.screens.added.empty() ||
        !m.screens.acfPatched.empty()) {
        json j = json::object();
        PutStrings(j, "added", m.screens.added);
        PutStrings(j, "acf_patched", m.screens.acfPatched);
        j["installed"] = m.screens.installed;
        j["version"] = m.screens.version;
        root["screens"] = j;
    }

    return root.dump(2);
}

bool Write(const std::string& objectsDirUtf8, const Manifest& m,
           std::string& errorOut) {
    const fs::path p = fsutil::PathFromUtf8(PathFor(objectsDirUtf8));
    std::error_code ec;
    if (!fsutil::AtomicWriteBytes(p, Serialize(m), ec)) {
        errorOut = "couldn't write " + fsutil::PathToUtf8(p) + " (" +
                   ec.message() +
                   ") — check X-Plane is closed and you have permission to write "
                   "there";
        return false;
    }
    return true;
}

}  // namespace manifest
}  // namespace photon
