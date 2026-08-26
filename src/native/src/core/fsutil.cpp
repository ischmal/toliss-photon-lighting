#include "core/fsutil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace photon {
namespace fsutil {
namespace {

constexpr std::size_t kCompareChunk = 1u << 16;

#ifdef _WIN32
std::wstring WideFromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                           static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), need);
    return out;
}

std::string Utf8FromWide(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, s.data(),
                                           static_cast<int>(s.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}
#endif

// A temp name in the DESTINATION's own directory. Same-volume is what makes the
// rename atomic; a system temp dir would silently degrade to a copy.
//
// ⚠ `+=` appends in the path's own native encoding. Building the name via
// `filename().string()` narrows through the ANSI code page on Windows, which
// mangles a non-ASCII filename (a flattened livery backup carries the livery
// folder's name, which the user chose) into an unwritable one — the same trap
// patch_realwings sidesteps for its `.bak` sibling.
fs::path TempSibling(const fs::path& dest) {
    fs::path tmp = dest;
    tmp += ".photontmp";
    return tmp;
}

}  // namespace

fs::path PathFromUtf8(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(WideFromUtf8(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string PathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return Utf8FromWide(p.wstring());
#else
    return p.string();
#endif
}

std::string PathToUtf8Generic(const fs::path& p) {
#ifdef _WIN32
    return Utf8FromWide(p.generic_wstring());
#else
    return p.generic_string();
#endif
}

bool ReadFileBytes(const fs::path& p, std::string& out) {
    std::error_code ec;
    return ReadFileBytes(p, out, ec);
}

bool ReadFileBytes(const fs::path& p, std::string& out, std::error_code& ec) {
    ec.clear();
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    if (in.bad()) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }
    return true;
}

bool AtomicWriteBytes(const fs::path& dest, const std::string& data) {
    std::error_code ec;
    return AtomicWriteBytes(dest, data, ec);
}

bool AtomicWriteBytes(const fs::path& dest, const std::string& data,
                      std::error_code& ec, int attempts, int retryDelayMs) {
    ec.clear();
    if (dest.has_parent_path()) {
        std::error_code mk;
        fs::create_directories(dest.parent_path(), mk);
    }
    const fs::path tmp = TempSibling(dest);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            ec = std::make_error_code(std::errc::permission_denied);
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code rm;
            fs::remove(tmp, rm);
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }
    }
    for (int attempt = 0; attempt < std::max(1, attempts); ++attempt) {
        std::error_code rn;
        fs::rename(tmp, dest, rn);
        if (!rn) return true;
        ec = rn;
        if (attempt + 1 < attempts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
        }
    }
    std::error_code rm;
    fs::remove(tmp, rm);
    return false;
}

bool FilesIdentical(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    const auto sa = fs::file_size(a, ec);
    if (ec) return false;
    const auto sb = fs::file_size(b, ec);
    if (ec) return false;
    if (sa != sb) return false;

    std::ifstream fa(a, std::ios::binary);
    std::ifstream fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    std::string bufA(kCompareChunk, '\0');
    std::string bufB(kCompareChunk, '\0');
    for (;;) {
        fa.read(bufA.data(), static_cast<std::streamsize>(kCompareChunk));
        fb.read(bufB.data(), static_cast<std::streamsize>(kCompareChunk));
        const std::streamsize na = fa.gcount();
        const std::streamsize nb = fb.gcount();
        if (na != nb) return false;
        if (na == 0) return true;
        if (std::memcmp(bufA.data(), bufB.data(),
                        static_cast<std::size_t>(na)) != 0) {
            return false;
        }
        if (fa.eof() && fb.eof()) return true;
        if (fa.bad() || fb.bad()) return false;
    }
}

bool IsSymlinkNoFollow(const fs::path& p) {
    std::error_code ec;
    // symlink_status does NOT follow, which is the entire point here.
    const auto st = fs::symlink_status(p, ec);
    if (ec) return false;
    return fs::is_symlink(st);
}

std::vector<FileEntry> ListFilesRecursive(const fs::path& root) {
    std::vector<FileEntry> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const fs::path rel = fs::relative(it->path(), root, fe);
        if (fe) continue;
        FileEntry entry;
        entry.rel = PathToUtf8Generic(rel);
        entry.abs = it->path();
        out.push_back(std::move(entry));
    }
    std::sort(out.begin(), out.end(),
              [](const FileEntry& a, const FileEntry& b) { return a.rel < b.rel; });
    return out;
}

void RemoveDirIfEmpty(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec) || ec) return;
    if (fs::directory_iterator(p, ec) == fs::directory_iterator()) {
        fs::remove(p, ec);
    }
}

std::string DetectNewline(const std::string& text) {
    return text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::string norm;
    norm.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
        norm.push_back(text[i]);
    }
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= norm.size(); ++i) {
        if (i == norm.size() || norm[i] == '\n') {
            out.push_back(norm.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::string JoinLines(const std::vector<std::string>& lines, const std::string& nl) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i) out += nl;
        out += lines[i];
    }
    return out;
}

std::vector<std::string> SplitLinesKeepEnds(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            out.push_back(text.substr(start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < text.size()) out.push_back(text.substr(start));
    return out;
}

std::string Trim(const std::string& s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    auto b = std::find_if(s.begin(), s.end(), notSpace);
    auto e = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    return (b < e) ? std::string(b, e) : std::string();
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ToLower(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool GlobMatch(const std::string& pattern, const std::string& text) {
    const std::string p = ToLower(pattern);
    const std::string t = ToLower(text);
    // Iterative backtracking match — no recursion, so a pathological pattern
    // cannot blow the stack on a folder name a user chose.
    std::size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

}  // namespace fsutil
}  // namespace photon
