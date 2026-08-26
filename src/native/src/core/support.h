// The support report — everything a user has to send when something goes wrong,
// collected into ONE file they can attach to a forum comment.
//
// ⚠ THE POINT IS THAT IT IS ONE FILE. "Please attach your Log.txt" already fails
// half the time; "please attach Log.txt, the previous three Log.N.txt, and the
// installer log for whichever aircraft you installed onto — which is in your
// temp folder under a name you do not know" fails every time. The plugin knows
// where all of those live, so it collects them itself.
//
// ⚠ NOTHING HERE INCLUDES AN XPLM HEADER. This is `photoncore`, which builds on a
// runner with no X-Plane SDK; the caller passes the X-Plane root in and does its
// own dataref reading. See core/constants.h.
//
// XML rather than a ZIP for one reason: no compressor is vendored, and the
// alternative (several loose files) is the problem this exists to solve. It is
// text, so a maintainer can read it in any editor without unpacking anything, and
// it survives being pasted into a forum post.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace photon {
namespace support {

// ⚠ WRITTEN BY THE INSTALLER, READ BY THE PLUGIN — see actions::Install. A run's
// log is a flat list of file operations and the only thing naming the aircraft is
// a path inside a "writing ..." line, so attributing a log to an airframe meant
// guessing from filenames. This tag states it: `install target: a321 ...`.
//
// ⚠ THE FALLBACK STAYS AND IS NOT DEAD CODE. Every log already on a user's disk
// predates the tag, and those are exactly the logs a first bug report carries.
// AirframesNamedIn falls back to the airframe FINGERPRINTS (Airframe::objName).
constexpr char kInstallTargetTag[] = "install target: ";

// The installer's log file name prefix, as FileLog builds it.
constexpr char kInstallerLogPrefix[] = "ToLissPhoton_install_";

// One file that goes into the report.
struct LogFile {
    std::string kind;        // "xplane" | "installer"
    std::string name;        // file name as it is on disk
    std::string note;        // "current session", "previous session 1", ...
    // Installer logs only: the airframe keys this file was chosen AS THE LATEST
    // FOR. Empty on the most-recent-run fallback, which is included even when it
    // names no airframe at all (a run that only detected, or that failed early).
    std::vector<std::string> airframes;
    std::string path;        // absolute, UTF-8
    std::uint64_t bytes = 0; // size on disk, before any excerpting
    std::string modified;    // "YYYY-MM-DD HH:MM:SS", local time
};

// ─── where things are ────────────────────────────────────────────────────────
// The OS temp directory, UTF-8 — where the installer writes its logs.
//
// ⚠ NOT `installer::platform::TempDir`. That lives in the installer target, which
// the `.xpl` does not link; this is the same answer reached through
// std::filesystem so both halves can ask it.
std::string TempDir();

// `<X-Plane>/Log.txt` plus up to `history` rotated `Log.N.txt`, newest first.
//
// ⚠ X-PLANE 12 DOES ROTATE ITS LOG — Log.1.txt .. Log.4.txt sit beside Log.txt —
// which is what makes "the last three sessions" answerable at all. A user who
// restarted the sim before reporting has the interesting run in Log.1.txt, not in
// Log.txt, and asking for a fresh Log.txt gets the run where nothing went wrong.
std::vector<LogFile> XPlaneLogs(const std::string& xplaneRootUtf8, int history = 3);

// The newest installer log FOR EACH AIRFRAME found under `tempDirUtf8`, plus the
// newest run overall, newest first and never listing one file twice.
//
// ⚠ PER AIRFRAME, not "the latest log". One run installs onto one aircraft, so
// the latest log alone is silent about the other two — and "it works on my A320
// but not my A321" is the shape most reports arrive in.
std::vector<LogFile> InstallerLogs(const std::string& tempDirUtf8);

// Which airframe keys a log's text refers to: the kInstallTargetTag lines if it
// has any, otherwise the airframes whose exterior-OBJ fingerprint it names.
// Deduped, in Airframes() order.
std::vector<std::string> AirframesNamedIn(const std::string& logText);

// ─── the report ──────────────────────────────────────────────────────────────
// One line of the report's header — whatever the caller can say about the
// machine, the aircraft and the plugin's own state.
struct ReportFact {
    std::string name;
    std::string value;
};

// A file's text as it goes into the report: whole, or its head and its tail with
// the middle elided.
//
// ⚠ HEAD *AND* TAIL, NEVER ONE OF THEM. A 13 MB Log.txt is ordinary (X-Plane logs
// every scenery load), and both ends carry different evidence: the system block —
// GPU, driver, plugin list, X-Plane build — is in the first few hundred lines, and
// whatever just went wrong is at the end. A tail-only excerpt loses which driver
// it was; a head-only one loses the error.
std::string Excerpt(const std::string& text, bool& truncated);

// Read a file for the report, applying Excerpt without loading the whole thing:
// the head and the tail are seeked to. `bytes` is the size ON DISK.
bool ReadForReport(const std::string& pathUtf8, std::string& out,
                   std::uint64_t& bytes, bool& truncated);

// The whole document, ready to write.
std::string BuildReportXml(const std::vector<ReportFact>& facts,
                           const std::vector<LogFile>& logs);

// `ToLissPhoton_Report_YYYYmmdd_HHMMSS.xml`.
std::string ReportFileName();

// Write `xml` into `dirUtf8`. Returns the full path written, or "" with a reason
// in `err`.
std::string WriteReport(const std::string& dirUtf8, const std::string& xml,
                        std::string& err);

// ─── XML plumbing ────────────────────────────────────────────────────────────
// Attribute/element text: the five predefined entities.
std::string XmlEscape(const std::string& s);

// A CDATA section around arbitrary log text.
//
// ⚠ TWO THINGS MAKE A LOG UNSAFE TO PASTE INTO XML AND BOTH ARE SILENT. `]]>`
// inside the text ENDS the section early (it is a byte sequence, not an escape,
// so nothing warns) — hence the split into two sections. And a log carries bytes
// that XML 1.0 forbids outright: control characters, and whatever invalid UTF-8 a
// mis-encoded scenery name left behind. Either makes the finished report refuse
// to open in the reader the maintainer reaches for, which is a worse outcome than
// the missing byte.
std::string XmlCdata(const std::string& s);

// Drop the bytes XML 1.0 cannot carry: C0 controls other than tab/CR/LF, and any
// byte that is not part of a well-formed UTF-8 sequence.
std::string SanitizeXmlText(const std::string& s);

// "YYYY-MM-DD HH:MM:SS" in local time, from a POSIX time.
std::string FormatTime(std::int64_t epochSeconds);

}  // namespace support
}  // namespace photon
