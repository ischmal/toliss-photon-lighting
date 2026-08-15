#include "core/config.h"

#include <cstdlib>

#include "core/fsutil.h"
#include "third_party/nlohmann/json.hpp"

namespace photon {
namespace config {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr char kConfigFilename[] = "installer_config.json";
constexpr char kAppDirName[] = "ToLissPhoton";

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string HomeDir() {
#ifdef _WIN32
    const std::string profile = Env("USERPROFILE");
    if (!profile.empty()) return profile;
    const std::string drive = Env("HOMEDRIVE");
    const std::string path = Env("HOMEPATH");
    if (!drive.empty() && !path.empty()) return drive + path;
    return std::string();
#else
    return Env("HOME");
#endif
}

}  // namespace

std::string ConfigDir() {
#ifdef _WIN32
    std::string base = Env("APPDATA");
    if (base.empty()) base = HomeDir();
    return fsutil::PathToUtf8(fsutil::PathFromUtf8(base) / kAppDirName);
#elif defined(__APPLE__)
    return fsutil::PathToUtf8(fsutil::PathFromUtf8(HomeDir()) / "Library" /
                              "Application Support" / kAppDirName);
#else
    std::string base = Env("XDG_CONFIG_HOME");
    if (base.empty()) {
        base = fsutil::PathToUtf8(fsutil::PathFromUtf8(HomeDir()) / ".config");
    }
    return fsutil::PathToUtf8(fsutil::PathFromUtf8(base) / kAppDirName);
#endif
}

std::string ConfigPath() {
    return fsutil::PathToUtf8(fsutil::PathFromUtf8(ConfigDir()) / kConfigFilename);
}

std::string LoadSavedRoot() {
    std::string text;
    if (!fsutil::ReadFileBytes(fsutil::PathFromUtf8(ConfigPath()), text)) {
        return std::string();
    }
    try {
        const json data = json::parse(text);
        const auto it = data.find("xplane_root");
        if (it != data.end() && it->is_string()) return it->get<std::string>();
    } catch (const json::exception&) {
        // Corrupt reads as "never saved" — the user is asked once and it is fixed.
    }
    return std::string();
}

void SaveRoot(const std::string& rootUtf8) {
    json data = json::object();
    data["xplane_root"] = rootUtf8;
    std::error_code ec;
    fsutil::AtomicWriteBytes(fsutil::PathFromUtf8(ConfigPath()), data.dump(), ec);
}

void ClearSavedRoot() {
    std::error_code ec;
    fs::remove(fsutil::PathFromUtf8(ConfigPath()), ec);
}

}  // namespace config
}  // namespace photon
