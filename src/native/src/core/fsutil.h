// Filesystem primitives shared by every patcher and by the installer's actions.
//
// ⚠ PATHS ARE UTF-8 END TO END. All strings crossing these functions are UTF-8;
// the conversion to the platform's native encoding happens once, inside
// `PathFromUtf8` / `PathToUtf8`. An aircraft folder named by a user in any
// language has to survive the scan, the manifest (JSON is UTF-8) and the TUI table
// — on Windows that means the wide API, never the ANSI code page.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace photon {
namespace fsutil {

namespace fs = std::filesystem;

// ─── UTF-8 <-> std::filesystem::path ─────────────────────────────────────────
fs::path PathFromUtf8(const std::string& utf8);
std::string PathToUtf8(const fs::path& p);
// The POSIX spelling of a (relative) path, UTF-8 — what a manifest entry, a step
// line or a flattened backup name is built from.
//
// ⚠ NEVER `generic_string()` FOR THIS. On MSVC it narrows through the ANSI code
// page: a livery folder named with an en dash or an é came back as CP1252 bytes
// that the next UTF-8 reader mangled (the manifest's JSON writer REJECTS them, so
// the install failed at its last step with the livery's texture already removed),
// and a character the page cannot spell at all — Turkish ı, Polish ł, Cyrillic,
// CJK — THREW std::system_error out of a loop half-way through moving a user's
// files. Found 2026-08-22 in the interior livery scan.
std::string PathToUtf8Generic(const fs::path& p);

// ─── reading ─────────────────────────────────────────────────────────────────
// ⚠ BINARY, ALWAYS. `.acf` files are CRLF on disk and `.obj` files may be either;
// any newline translation on the way in or out is a gratuitous whole-file diff in
// a file other mods also read and back up. The Python side spells this
// `newline=""` on both sides; in C++ it is simply "read bytes, never touch line
// endings, write bytes".
bool ReadFileBytes(const fs::path& p, std::string& out);
bool ReadFileBytes(const fs::path& p, std::string& out, std::error_code& ec);

// ─── writing ─────────────────────────────────────────────────────────────────
// Same-directory temp file + atomic rename, so a crash or a permission error can
// never leave a half-written OBJ in place and a concurrent reader always sees the
// complete old file or the complete new one.
//
// ⚠ `attempts` is not paranoia. Windows transiently denies a rename over a file
// another process still has open for reading (no FILE_SHARE_DELETE) — the expected,
// self-resolving case when X-Plane is reading an OBJ we are rewriting. Failures are
// retried briefly rather than surfaced.
bool AtomicWriteBytes(const fs::path& dest, const std::string& data,
                      std::error_code& ec, int attempts = 20,
                      int retryDelayMs = 150);
bool AtomicWriteBytes(const fs::path& dest, const std::string& data);

// ─── comparison ──────────────────────────────────────────────────────────────
// Byte-equality that avoids reading contents whenever it can: a size check first
// (a different plugin build almost always differs in size, so the common upgrade
// case is decided from stat alone), then a chunked compare that stops at the first
// differing block.
//
// ⚠ UNREADABLE OR MISSING COUNTS AS "NOT IDENTICAL". Callers treat that as
// needs-(re)write, which is the safe direction: the alternative would skip writing
// a file we could not prove was already correct.
bool FilesIdentical(const fs::path& a, const fs::path& b);

// ─── symlinks ────────────────────────────────────────────────────────────────
// ⚠ ON THE PATH ITSELF, NEVER AFTER FOLLOWING IT. A dev keeps `panelfx.txt`
// symlinked at their repo copy so in-sim edits land in source control; a link whose
// target has moved reads as "not a file" and would otherwise fall through to the
// delete — taking the link with it for the one reason it was worth keeping.
bool IsSymlinkNoFollow(const fs::path& p);

// ─── directories ─────────────────────────────────────────────────────────────
// Every regular file under `root`, as (root-relative POSIX path, absolute path),
// sorted by the relative path so callers are deterministic.
struct FileEntry {
    std::string rel;   // POSIX-separated, relative to root
    fs::path abs;
};
std::vector<FileEntry> ListFilesRecursive(const fs::path& root);

// Remove a directory only if it is now empty. Never an error if it is not.
void RemoveDirIfEmpty(const fs::path& p);

// ─── text helpers used by the marker and the patchers ────────────────────────
// The newline flavor a text blob already uses, so a rewrite preserves it.
std::string DetectNewline(const std::string& text);

// Split on '\n' after normalizing CRLF away. Pair with `DetectNewline` + `Join`.
std::vector<std::string> SplitLines(const std::string& text);
std::string JoinLines(const std::vector<std::string>& lines, const std::string& nl);

// Split KEEPING each line's own terminator, so a file with mixed endings survives
// a property rewrite byte-for-byte. This is what the `.acf` engine uses.
std::vector<std::string> SplitLinesKeepEnds(const std::string& text);

std::string Trim(const std::string& s);
bool StartsWith(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);
std::string ToLower(const std::string& s);

// A case-insensitive glob with `*` and `?` only — enough for the airframe folder
// globs ("ToLissA319*"), which is the one place we match a name rather than
// content.
bool GlobMatch(const std::string& pattern, const std::string& text);

}  // namespace fsutil
}  // namespace photon
