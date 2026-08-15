#include "core/progress.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include "core/fsutil.h"

namespace photon {
namespace progress {
namespace {

namespace fs = std::filesystem;

constexpr double kMiB = 1048576.0;

// The measured table in progress.h, in one place each.
constexpr double kMsPerMiBObjScan = 3.5;
constexpr double kMsPerMiBObjPatch = 10.5;
constexpr double kMsPerMiBAcfAttach = 38.0;
constexpr double kMsPerMiBAcfSpots = 28.0;
constexpr double kMsPerMiBCopy = 2.0;

double Mib(std::uint64_t bytes) { return static_cast<double>(bytes) / kMiB; }

UnknownStepHook gUnknownStepHook;

}  // namespace

void SetUnknownStepHook(UnknownStepHook hook) { gUnknownStepHook = std::move(hook); }

double MsForObjScan(std::uint64_t bytes) { return Mib(bytes) * kMsPerMiBObjScan; }
double MsForObjPatch(std::uint64_t bytes) { return Mib(bytes) * kMsPerMiBObjPatch; }
double MsForAcfAttach(std::uint64_t bytes) { return Mib(bytes) * kMsPerMiBAcfAttach; }
double MsForAcfSpots(std::uint64_t bytes) { return Mib(bytes) * kMsPerMiBAcfSpots; }
double MsForCopy(std::uint64_t bytes) { return Mib(bytes) * kMsPerMiBCopy; }

namespace {

template <typename Iter>
std::uint64_t SumWith(Iter it, const std::string& want, std::error_code& ec) {
    std::uint64_t total = 0;
    for (Iter end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe) continue;
        const std::string name =
            fsutil::ToLower(fsutil::PathToUtf8(it->path().filename()));
        if (!fsutil::EndsWith(name, want)) continue;
        // ⚠ `file_size` on the ENTRY, not a second stat of the path — the iterator
        // already carries it on Windows, and on a 1.6 GB folder of 61 files that
        // difference is most of the cost of planning.
        const std::uintmax_t sz = it->file_size(fe);
        if (!fe) total += static_cast<std::uint64_t>(sz);
    }
    return total;
}

}  // namespace

std::uint64_t BytesOfFilesWithExt(const std::string& dirUtf8,
                                  const std::string& ext, bool recursive) {
    const fs::path dir = fsutil::PathFromUtf8(dirUtf8);
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return 0;

    const std::string want = fsutil::ToLower(ext);
    ec.clear();
    return recursive
               ? SumWith(fs::recursive_directory_iterator(dir, ec), want, ec)
               : SumWith(fs::directory_iterator(dir, ec), want, ec);
}

std::uint64_t FileSizeOrZero(const std::string& pathUtf8) {
    std::error_code ec;
    const std::uintmax_t sz = fs::file_size(fsutil::PathFromUtf8(pathUtf8), ec);
    return ec ? 0 : static_cast<std::uint64_t>(sz);
}

void Plan::Add(const std::string& name, double estimatedMs) {
    // A negative estimate is a bug in a caller's arithmetic, not a reason to make
    // the bar run backwards — and every step carries the floor. See the header.
    const double ms =
        kMsStepFloor + (estimatedMs > 0.0 ? estimatedMs : 0.0);
    steps_.push_back(Step{name, ms});
    totalMs_ += ms;
}

void Plan::Begin(const std::string& name) {
    // Entering a step implicitly finishes the one before it. Callers that pair
    // Begin/Finish get the same answer; callers that just walk Begin to Begin do
    // not silently lose the banked time of the step they left.
    if (inStep_) Finish();
    for (const Step& s : steps_) {
        if (s.name != name) continue;
        currentMs_ = s.ms;
        withinMs_ = 0.0;
        inStep_ = true;
        return;
    }
    // Unknown name: deliberately not fatal in production, and reported to whoever
    // is watching. See SetUnknownStepHook.
    if (gUnknownStepHook) gUnknownStepHook(name);
    currentMs_ = 0.0;
    withinMs_ = 0.0;
    inStep_ = false;
}

void Plan::Within(double fraction) {
    if (!inStep_) return;
    const double f = std::min(1.0, std::max(0.0, fraction));
    // ⚠ MAX, NEVER ASSIGN. A patcher that reports its files out of order — or one
    // that reports a fraction of BYTES while a later file is smaller — would
    // otherwise walk the bar backwards inside a single step.
    withinMs_ = std::max(withinMs_, currentMs_ * f);
}

void Plan::Finish() {
    if (!inStep_) return;
    doneMs_ += currentMs_;
    currentMs_ = 0.0;
    withinMs_ = 0.0;
    inStep_ = false;
}

double Plan::Fraction() const {
    // ⚠ AN EMPTY OR ZERO-COST PLAN REPORTS 0, NOT 1. Nothing has been done yet, and
    // a bar that opens at 100% is the same lie as one that opens at 0% and stays.
    // The run's own completion sets 1.0 when it is genuinely finished.
    if (totalMs_ <= 0.0) return 0.0;
    const double f = (doneMs_ + withinMs_) / totalMs_;
    return std::min(1.0, std::max(0.0, f));
}

}  // namespace progress
}  // namespace photon
