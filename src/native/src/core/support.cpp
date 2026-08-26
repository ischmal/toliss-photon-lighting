#include "core/support.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "core/constants.h"
#include "core/fsutil.h"

namespace photon {
namespace support {

namespace fs = fsutil::fs;

namespace {

// How much of an over-long log survives. See Excerpt's note in the header for why
// it is both ends and not one: the system block is at the top and the failure is
// at the bottom, and a 13 MB Log.txt is an ordinary size.
constexpr std::size_t kHeadBytes = 256u * 1024u;
constexpr std::size_t kTailBytes = 1024u * 1024u;

// A log we will not even scan for airframe names. Installer logs are a few KB;
// anything this large is not one, and reading it whole to classify it would cost
// more than the report itself.
constexpr std::uint64_t kMaxInstallerLogBytes = 8u * 1024u * 1024u;

std::int64_t ToEpoch(fs::file_time_type t) {
    // C++17 has no portable file_clock -> system_clock conversion; this is the
    // usual shift through both clocks' "now". Good to the second, which is all a
    // human-readable stamp and a newest-first sort need.
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (std::int64_t)std::chrono::system_clock::to_time_t(sys);
}

std::uint64_t FileSize(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0u : (std::uint64_t)n;
}

// Cut back to a line boundary so an excerpt never starts or ends mid-line — a
// half line at a seam reads as corruption in the report rather than as a cut.
std::string TrimToLineStart(const std::string& s) {
    const std::size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(nl + 1);
}
std::string TrimToLineEnd(const std::string& s) {
    const std::size_t nl = s.rfind('\n');
    return nl == std::string::npos ? s : s.substr(0, nl + 1);
}

std::string ElisionMarker(std::uint64_t omitted) {
    return "\n\n===== [ToLiss Photon] " + std::to_string(omitted) +
           " bytes omitted from the middle of this log =====\n\n";
}

// The length of the UTF-8 sequence starting at s[i], or 0 if it is not one.
std::size_t Utf8SeqLen(const std::string& s, std::size_t i) {
    const unsigned char c = (unsigned char)s[i];
    std::size_t need = 0;
    if (c < 0x80) return 1;
    else if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else return 0;
    if (i + need > s.size()) return 0;
    for (std::size_t k = 1; k < need; ++k)
        if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 0;
    // Overlong forms and surrogates are ill-formed too, and a reader that rejects
    // them rejects the whole document.
    if (need == 2 && c < 0xC2) return 0;
    if (need == 3 && c == 0xE0 && (unsigned char)s[i + 1] < 0xA0) return 0;
    if (need == 3 && c == 0xED && (unsigned char)s[i + 1] >= 0xA0) return 0;
    if (need == 4 && (c > 0xF4 || (c == 0xF0 && (unsigned char)s[i + 1] < 0x90) ||
                      (c == 0xF4 && (unsigned char)s[i + 1] >= 0x90)))
        return 0;
    return need;
}

std::string StampCompact() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    if (localtime_s(&tmv, &t) != 0) return "unknown";
#else
    if (localtime_r(&t, &tmv) == nullptr) return "unknown";
#endif
    char buf[32] = {0};
    if (std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv) == 0) return "unknown";
    return buf;
}

}  // namespace

// ─── time ────────────────────────────────────────────────────────────────────
std::string FormatTime(std::int64_t epochSeconds) {
    const std::time_t t = (std::time_t)epochSeconds;
    std::tm tmv{};
#if defined(_WIN32)
    if (localtime_s(&tmv, &t) != 0) return std::string();
#else
    if (localtime_r(&t, &tmv) == nullptr) return std::string();
#endif
    char buf[32] = {0};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv) == 0)
        return std::string();
    return buf;
}

// ─── where things are ────────────────────────────────────────────────────────
std::string TempDir() {
    std::error_code ec;
    const fs::path p = fs::temp_directory_path(ec);
    if (ec) return std::string();
    return fsutil::PathToUtf8(p);
}

std::vector<LogFile> XPlaneLogs(const std::string& xplaneRootUtf8, int history) {
    std::vector<LogFile> out;
    if (xplaneRootUtf8.empty()) return out;
    const fs::path root = fsutil::PathFromUtf8(xplaneRootUtf8);

    // Log.txt, then Log.1.txt .. Log.<history>.txt. X-Plane rotates on start, so
    // the numbering is already newest-first and needs no sort.
    for (int i = 0; i <= (history < 0 ? 0 : history); ++i) {
        const std::string name =
            i == 0 ? std::string("Log.txt")
                   : ("Log." + std::to_string(i) + ".txt");
        const fs::path p = root / name;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) continue;
        LogFile f;
        f.kind = "xplane";
        f.name = name;
        f.note = i == 0 ? std::string("current session")
                        : ("previous session " + std::to_string(i));
        f.path = fsutil::PathToUtf8(p);
        f.bytes = FileSize(p);
        std::error_code tec;
        f.modified = FormatTime(ToEpoch(fs::last_write_time(p, tec)));
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<std::string> AirframesNamedIn(const std::string& logText) {
    // ⚠ THE TAG WINS OUTRIGHT WHERE IT IS PRESENT. The fingerprint fallback below
    // reads `ExternalLights_XP12.obj` as an A330-900, and that is ToLiss's name
    // for EVERY newer airframe's exterior lights OBJ — so an A340 the installer
    // rightly refused would otherwise be reported as an installed a339.
    std::vector<std::string> tagged;
    const std::size_t tagLen = std::char_traits<char>::length(kInstallTargetTag);
    std::size_t at = logText.find(kInstallTargetTag);
    while (at != std::string::npos) {
        const std::size_t start = at + tagLen;
        std::size_t end = start;
        while (end < logText.size() && logText[end] != ' ' && logText[end] != '\r' &&
               logText[end] != '\n' && logText[end] != '\t')
            ++end;
        const std::string key = logText.substr(start, end - start);
        if (AirframeByKey(key) != nullptr &&
            std::find(tagged.begin(), tagged.end(), key) == tagged.end())
            tagged.push_back(key);
        at = logText.find(kInstallTargetTag, end);
    }

    std::vector<std::string> out;
    for (const Airframe& af : Airframes()) {
        const bool hit =
            tagged.empty()
                ? logText.find(af.objName) != std::string::npos
                : std::find(tagged.begin(), tagged.end(), af.key) != tagged.end();
        if (hit) out.push_back(af.key);
    }
    return out;
}

std::vector<LogFile> InstallerLogs(const std::string& tempDirUtf8) {
    std::vector<LogFile> out;
    if (tempDirUtf8.empty()) return out;
    const fs::path dir = fsutil::PathFromUtf8(tempDirUtf8);
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;

    struct Candidate {
        fs::path path;
        std::string name;
        std::int64_t epoch = 0;
        std::uint64_t bytes = 0;
        std::vector<std::string> airframes;
    };
    std::vector<Candidate> found;

    // ⚠ ONE LEVEL, AND NEVER A RECURSIVE WALK. This is the OS temp folder: on a
    // real machine it holds thousands of files and other installers' unpacked
    // trees, and recursing it would turn a support click into a disk scan.
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied,
                                   ec),
         end;
         !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        const std::string name = fsutil::PathToUtf8(it->path().filename());
        if (!fsutil::StartsWith(name, kInstallerLogPrefix)) continue;
        if (!fsutil::EndsWith(fsutil::ToLower(name), ".log")) continue;
        Candidate c;
        c.path = it->path();
        c.name = name;
        c.bytes = FileSize(c.path);
        std::error_code tec;
        c.epoch = ToEpoch(fs::last_write_time(c.path, tec));
        if (c.bytes <= kMaxInstallerLogBytes) {
            std::string text;
            if (fsutil::ReadFileBytes(c.path, text)) c.airframes = AirframesNamedIn(text);
        }
        found.push_back(std::move(c));
    }
    if (found.empty()) return out;

    // Newest first. The file NAME carries a sortable stamp too, but a copied or
    // restored log keeps its name and loses its stamp, so mtime is the truth.
    std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) {
        if (a.epoch != b.epoch) return a.epoch > b.epoch;
        return a.name > b.name;
    });

    // For each airframe, the newest log naming it — plus the newest run of all,
    // which is what a report about an install that never got as far as naming an
    // aircraft has to carry. Marked by index and emitted in newest-first order, so
    // one file chosen for two airframes still appears exactly once.
    std::vector<bool> selected(found.size(), false);
    std::vector<std::vector<std::string>> keys(found.size());
    for (const Airframe& af : Airframes()) {
        for (std::size_t i = 0; i < found.size(); ++i) {
            if (std::find(found[i].airframes.begin(), found[i].airframes.end(),
                          af.key) == found[i].airframes.end())
                continue;
            selected[i] = true;
            keys[i].push_back(af.key);
            break;
        }
    }
    selected[0] = true;   // the most recent run, whatever it touched

    for (std::size_t i = 0; i < found.size(); ++i) {
        if (!selected[i]) continue;
        LogFile f;
        f.kind = "installer";
        f.name = found[i].name;
        f.airframes = keys[i];
        f.note = keys[i].empty() ? std::string("most recent installer run")
                                 : std::string("latest install log for this aircraft");
        f.path = fsutil::PathToUtf8(found[i].path);
        f.bytes = found[i].bytes;
        f.modified = FormatTime(found[i].epoch);
        out.push_back(std::move(f));
    }
    return out;
}

// ─── excerpting ──────────────────────────────────────────────────────────────
std::string Excerpt(const std::string& text, bool& truncated) {
    truncated = false;
    if (text.size() <= kHeadBytes + kTailBytes) return text;
    truncated = true;
    const std::string head = TrimToLineEnd(text.substr(0, kHeadBytes));
    const std::string tail = TrimToLineStart(text.substr(text.size() - kTailBytes));
    const std::uint64_t omitted =
        (std::uint64_t)text.size() - head.size() - tail.size();
    return head + ElisionMarker(omitted) + tail;
}

bool ReadForReport(const std::string& pathUtf8, std::string& out,
                   std::uint64_t& bytes, bool& truncated) {
    out.clear();
    bytes = 0;
    truncated = false;
    std::ifstream in(fsutil::PathFromUtf8(pathUtf8),
                     std::ios::in | std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    bytes = (std::uint64_t)size;

    // ⚠ SEEK, NEVER READ-THEN-CUT. Log.txt is routinely 10-15 MB and this runs on
    // the simulator thread from a button; pulling the whole file in to throw most
    // of it away is a visible stall for nothing.
    if (bytes <= (std::uint64_t)(kHeadBytes + kTailBytes)) {
        out.resize((std::size_t)bytes);
        in.seekg(0);
        if (bytes) in.read(&out[0], (std::streamsize)bytes);
        out.resize((std::size_t)in.gcount());
        return true;
    }
    truncated = true;
    std::string head(kHeadBytes, '\0');
    in.seekg(0);
    in.read(&head[0], (std::streamsize)kHeadBytes);
    head.resize((std::size_t)in.gcount());
    head = TrimToLineEnd(head);

    std::string tail(kTailBytes, '\0');
    in.clear();
    in.seekg((std::streamoff)(bytes - kTailBytes));
    in.read(&tail[0], (std::streamsize)kTailBytes);
    tail.resize((std::size_t)in.gcount());
    tail = TrimToLineStart(tail);

    out = head + ElisionMarker(bytes - head.size() - tail.size()) + tail;
    return true;
}

// ─── XML ─────────────────────────────────────────────────────────────────────
std::string SanitizeXmlText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20) {
            // Tab, LF and CR are the three C0 characters XML 1.0 allows; the rest
            // are DROPPED rather than replaced, so a log that has gone binary does
            // not come back as a wall of '?'.
            if (c == '\t' || c == '\n' || c == '\r') out += (char)c;
            ++i;
            continue;
        }
        if (c == 0x7F) { ++i; continue; }
        const std::size_t n = Utf8SeqLen(s, i);
        if (n == 0) { out += '?'; ++i; continue; }
        out.append(s, i, n);
        i += n;
    }
    return out;
}

std::string XmlEscape(const std::string& s) {
    const std::string clean = SanitizeXmlText(s);
    std::string out;
    out.reserve(clean.size() + clean.size() / 8);
    for (char ch : clean) {
        switch (ch) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case 0x27: out += "&apos;"; break;
            default:   out += ch;       break;
        }
    }
    return out;
}

std::string XmlCdata(const std::string& s) {
    const std::string clean = SanitizeXmlText(s);
    std::string out = "<![CDATA[";
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = clean.find("]]>", from);
        if (at == std::string::npos) {
            out.append(clean, from, std::string::npos);
            break;
        }
        // Close between the ']]' and the '>', then reopen: the '>' becomes the
        // first byte of the next section and the sequence never appears whole.
        out.append(clean, from, at + 2 - from);
        out += "]]><![CDATA[";
        from = at + 2;
    }
    out += "]]>";
    return out;
}

std::string ReportFileName() {
    return "ToLissPhoton_Report_" + StampCompact() + ".xml";
}

std::string BuildReportXml(const std::vector<ReportFact>& facts,
                           const std::vector<LogFile>& logs) {
    std::string x;
    x.reserve(1024u * 1024u);
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    x += "<!-- ToLiss Photon support report. Attach this file to your bug report;\n"
         "     it carries X-Plane's own logs and the installer's, and nothing else. -->\n";
    x += "<photonReport version=\"" + XmlEscape(kPhotonVersion) + "\" generated=\"" +
         XmlEscape(FormatTime((std::int64_t)std::time(nullptr))) + "\">\n";

    x += "  <about>\n";
    for (const ReportFact& f : facts)
        x += "    <fact name=\"" + XmlEscape(f.name) + "\">" + XmlEscape(f.value) +
             "</fact>\n";
    x += "  </about>\n";

    x += "  <logs count=\"" + std::to_string(logs.size()) + "\">\n";
    for (const LogFile& f : logs) {
        std::string text;
        std::uint64_t bytes = f.bytes;
        bool truncated = false;
        const bool ok = ReadForReport(f.path, text, bytes, truncated);

        x += "    <log kind=\"" + XmlEscape(f.kind) + "\" name=\"" +
             XmlEscape(f.name) + "\"";
        if (!f.note.empty()) x += " note=\"" + XmlEscape(f.note) + "\"";
        if (!f.airframes.empty()) {
            std::string joined;
            for (const std::string& k : f.airframes) {
                if (!joined.empty()) joined += ",";
                joined += k;
            }
            x += " aircraft=\"" + XmlEscape(joined) + "\"";
        }
        x += " bytes=\"" + std::to_string(bytes) + "\"";
        if (!f.modified.empty()) x += " modified=\"" + XmlEscape(f.modified) + "\"";
        x += " excerpt=\"";
        x += truncated ? "head+tail" : "complete";
        x += "\">\n";
        x += "      <path>" + XmlEscape(f.path) + "</path>\n";
        if (ok) x += "      <content>" + XmlCdata(text) + "</content>\n";
        else    x += "      <content unreadable=\"true\"/>\n";
        x += "    </log>\n";
    }
    x += "  </logs>\n";
    x += "</photonReport>\n";
    return x;
}

std::string WriteReport(const std::string& dirUtf8, const std::string& xml,
                        std::string& err) {
    err.clear();
    if (dirUtf8.empty()) {
        err = "no folder to write to";
        return std::string();
    }
    const fs::path dest = fsutil::PathFromUtf8(dirUtf8) / ReportFileName();
    std::error_code ec;
    if (!fsutil::AtomicWriteBytes(dest, xml, ec)) {
        err = ec ? ec.message() : std::string("could not write the file");
        return std::string();
    }
    return fsutil::PathToUtf8(dest);
}

}  // namespace support
}  // namespace photon
