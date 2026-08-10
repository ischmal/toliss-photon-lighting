#include "installer/log.h"

#include "core/fsutil.h"
#include "core/version.h"
#include "installer/platform.h"

namespace photon {
namespace installer {

FileLog::FileLog(const std::string& version, bool dryRun) {
    path_ = fsutil::PathToUtf8(fsutil::PathFromUtf8(platform::TempDir()) /
                               ("ToLissPhoton_install_" +
                                platform::TimestampCompact() + ".log"));
    // Binary so the CRLF the Windows text mode would inject cannot turn a log
    // shared between platforms into a mixed-ending file; the lines carry their own
    // '\n'. An unopenable log is not a reason to fail — Write() checks.
    out_.open(fsutil::PathFromUtf8(path_), std::ios::out | std::ios::binary |
                                               std::ios::trunc);
    Write("ToLiss Photon " + version + " installer log — " +
          platform::TimestampHuman());
    Write("platform: " + platform::PlatformName() + "  installer: native (C++)");
    if (dryRun) Write("MODE: --dry-run — no files will be modified");
    Write(std::string(70, '-'));
}

void FileLog::Write(const std::string& msg, const std::string& level) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out_.is_open()) return;
    std::string lvl = level;
    lvl.resize(5, ' ');
    out_ << platform::TimestampClock() << " [" << lvl << "] " << msg << "\n";
    out_.flush();
}

}  // namespace installer
}  // namespace photon
