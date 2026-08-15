// The verbose installer log: timestamped, leveled lines written to a file in the
// OS temp directory and FLUSHED ON EVERY WRITE, so a crash or a hard kill still
// leaves a full trace of how far the run got. Opened in the OS text viewer from
// the Complete screen.
//
// Implements `actions::Log`, so the same object serves the screens and the
// install/uninstall machinery.
#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "core/actions.h"

namespace photon {
namespace installer {

class FileLog : public actions::Log {
public:
    FileLog(const std::string& version, bool dryRun);

    // ⚠ LOCKED. The Perform screen runs the filesystem work on a worker thread
    // while the main thread animates — both write here. The Python version leans
    // on the GIL for this; C++ has to say it.
    void Write(const std::string& msg, const std::string& level = "INFO") override;

    const std::string& Path() const { return path_; }

private:
    std::string path_;
    std::ofstream out_;
    std::mutex mu_;
};

}  // namespace installer
}  // namespace photon
