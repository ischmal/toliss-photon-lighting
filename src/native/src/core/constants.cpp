#include "core/constants.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace photon {
namespace {

std::string LowerCopy(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool Contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

const std::vector<Airframe>& Airframes() {
    static const std::vector<Airframe> kAirframes = {
        {"a319", "ToLissA319*", "A319", "lights_out319_XP12.obj",
         "ToLiss A319", "/A319/", "A319"},
        {"a320", "ToLissA320*", "A320", "lights_out320_XP12.obj",
         "ToLiss A320", "/A320/", "A320"},
        {"a321", "ToLissA321*", "A321", "lights_out321_XP12.obj",
         "ToLiss A321", "/A321/", "A321"},
        {"a339", "ToLissA339*", "A339", "ExternalLights_XP12.obj",
         "ToLiss A330-900", "/A330-900/", "A330-900"},
    };
    return kAirframes;
}

const Airframe* AirframeByKey(const std::string& key) {
    for (const Airframe& af : Airframes()) {
        if (key == af.key) return &af;
    }
    return nullptr;
}

const std::vector<std::string>& InteriorAirframes() {
    static const std::vector<std::string> kKeys = {"a319", "a320", "a321"};
    return kKeys;
}

bool AirframeHasInterior(const std::string& key) {
    return Contains(InteriorAirframes(), key);
}

const std::vector<std::string>& InteriorTextures() {
    // Ordered by name so the install progress display is stable run to run.
    // NINE of these replace a stock ToLiss file and are restored from backup on
    // uninstall; the two in InteriorTextureIsAdded have no stock counterpart.
    static const std::vector<std::string> kTextures = {
        "chairs_LIT.png",
        "kitchens_LIT.png",
        "knobs.png",
        "knobs_LIT.png",
        "panels_overhead_LIT.png",
        "pedals_details_1.png",
        "pedals_details_2.png",
        "text_LIT.png",
        "walls_bottom_LIT.png",
        "walls_outer_LIT.png",
        "walls_top_LIT.png",
    };
    return kTextures;
}

bool InteriorTextureIsAdded(const std::string& name) {
    return name == "pedals_details_1.png" || name == "pedals_details_2.png";
}

const std::vector<std::string>& InteriorLiveryExts() {
    static const std::vector<std::string> kExts = {".dds", ".png"};
    return kExts;
}

const std::vector<std::string>& AcfSuffixes() {
    static const std::vector<std::string> kSuffixes = {
        "", "_StdDef", "_XP11", "_XP11_StdDef"};
    return kSuffixes;
}

const std::vector<std::string>& ScreensAirframes() {
    // A339 is out for the same reason it is out of the interior — a different
    // cockpit model, so these six display positions are simply not its.
    static const std::vector<std::string> kKeys = {"a319", "a320", "a321"};
    return kKeys;
}

bool AirframeHasScreens(const std::string& key) {
    return Contains(ScreensAirframes(), key);
}

const std::vector<GlowMapEntry>& GlowRedirect() {
    // A339: strobe [12]=L wingtip, [13]=R wingtip, [14]=tail; beacon [15],[16].
    // ⚠ MUST match GlowMapForIcao in plugin.cpp — which now reads this table, so
    // the agreement is structural rather than remembered. Airframes absent here
    // are not patched and keep ToLiss's own glow fade.
    static const std::vector<GlowMapEntry> kRedirect = {
        {"a339", {12, 13, 14, 15, 16}},
    };
    return kRedirect;
}

const std::vector<int>* GlowIndicesFor(const std::string& airframeKey) {
    for (const GlowMapEntry& e : GlowRedirect()) {
        if (airframeKey == e.airframeKey) return &e.indices;
    }
    return nullptr;
}

const std::vector<std::string>& Wings() {
    static const std::vector<std::string> kWings = {"stock", "durantula", "realwings"};
    return kWings;
}

const std::vector<std::string>& WingsFor(const std::string& airframeKey) {
    static const std::vector<std::string> kAll = {"stock", "durantula", "realwings"};
    static const std::vector<std::string> kStockOnly = {"stock"};
    static const std::vector<std::string> kNone = {};
    if (airframeKey == "a319" || airframeKey == "a320" || airframeKey == "a321") {
        return kAll;
    }
    if (airframeKey == "a339") return kStockOnly;
    return kNone;
}

bool WingIsOfferedFor(const std::string& airframeKey, const std::string& wing) {
    return Contains(WingsFor(airframeKey), wing);
}

std::string WingLabel(const std::string& wing) {
    if (wing == "stock") return "Default";
    if (wing == "durantula") return "Durantula";
    if (wing == "realwings") return "RealWings";
    return wing;
}

std::string WingActionLabel(const std::string& wing) {
    if (wing == "stock") return "Install";
    if (wing == "durantula") return "Install for Durantula's Wing Mod";
    if (wing == "realwings") return "Install for RealWings Mod";
    return "Install";
}

std::string RealWingsDir(const std::string& airframeKey) {
    if (airframeKey == "a319") return "RealWings319";
    if (airframeKey == "a320") return "RealWings320";
    if (airframeKey == "a321") return "RealWings321";
    return {};   // a339 and anything else: no wing mods at all
}

const std::vector<std::string>& PluginUserDirs() {
    // `overlays/` is the Panel FX compositor's image folder.
    static const std::vector<std::string> kDirs = {"overlays"};
    return kDirs;
}

bool PluginPathIsUserOwned(const std::string& relPosix) {
    // Matched on the first path SEGMENT, so a `.xpl` named `overlays.xpl` is not
    // caught by a prefix test.
    const std::size_t slash = relPosix.find('/');
    const std::string head =
        (slash == std::string::npos) ? relPosix : relPosix.substr(0, slash);
    return Contains(PluginUserDirs(), head);
}

std::string InteriorOrgUrl(const std::string& airframeKey) {
    static const std::map<std::string, std::string> kUrls = {
        {"a319", "https://forums.x-plane.org/files/file/93336-a319-light-mod/"},
        {"a320", "https://forums.x-plane.org/files/file/93337-a320-light-mod/"},
        {"a321", "https://forums.x-plane.org/files/file/93338-a321-light-mod/"},
    };
    const auto it = kUrls.find(LowerCopy(airframeKey));
    return it == kUrls.end() ? std::string() : it->second;
}

}  // namespace photon
