// What the installer's progress bar is made of.
//
// ⚠ THE BAR USED TO REPORT ONE LOOP OUT OF NINE PHASES. `ProgressFn` was added for
// the interior texture copy and nothing else ever called it, so an install without
// the cockpit mod — the common case, and the only case on the A339 — sat at 0% until
// `set_progress(1.0f)` fired at the end. With the cockpit mod it was worse, not
// better: the bar ran to 100% during a copy that is step 2 of 5 of the LAST phase,
// then sat at 100% while the livery scan and the four-`.acf` patch ran on.
//
// ⚠ A FIXED WEIGHT TABLE CANNOT WORK HERE, and that is the whole reason this file
// exists. The dominant cost is reading the AIRCRAFT's files, not writing ours, and
// aircraft differ enormously: `objects/*.obj` is 233 MB on the A319 and 1,597 MB on
// the A330-900. Measured end to end, a dry-run install is 0.50 s on the A320 and
// 11.5 s on the A339 — the same phases in the same order, 23x apart. Weights that
// are right for one are off by a factor of twenty on the other.
//
// So the plan is BUILT PER RUN FROM FILE SIZES, which `stat` answers in microseconds
// without reading a byte, and converted to milliseconds with the rates below.
//
// ─── the rates, and how they were measured ──────────────────────────────────
//
// 2026-08-13, this machine (NVMe, warm cache), dry-run installs with per-phase
// timers, against the four real ToLiss airframes. Every rate is (measured ms) /
// (bytes the phase actually touches):
//
//   kind             evidence                                        ms/MiB
//   ---------------- ----------------------------------------------- ------
//   obj token scan   a339 glow: 1,597 MiB x2 passes in 11,131 ms        3.5
//   obj patch        a320 realwings: 140.6 MiB in 1,479 ms             10.5
//   acf attach       a320 4x.acf 9.21 MiB in 346 ms;                   37.6
//                    a339 4x.acf 4.52 MiB in 176 ms                    38.9
//   acf spot patch   a320 4x.acf 9.21 MiB in 258 ms                    28.0
//   byte copy        59 MiB payload -> same volume in 118 ms            2.0
//
// ⚠ THE ABSOLUTE NUMBERS DO NOT MATTER AND THE RATIOS DO. The bar shows
// `done / total`, so any rate scaled by a constant gives the identical bar — what a
// slower disk changes is how long the bar takes, not where it sits. What WOULD skew
// it is the ratio between an I/O-bound phase (copy) and a CPU-bound one (the `.acf`
// parse, which is 19x dearer per byte because it reads every number and re-renders
// it in the file's own float style). Those two move differently across machines, and
// that is the known and accepted imprecision here. It is a progress bar.
//
// ⚠ RE-MEASURE WITH `docs/installer_gui_plan.md` §7's recipe, not by intuition. The
// two surprises in the table above were both counter-intuitive: the `.acf` parse is
// dearer per byte than anything else by an order of magnitude, and the glow scan —
// which reads a gigabyte and a half — is the cheapest per byte of the lot.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace photon {
namespace progress {

// How far through its own work one step is, 0..1. Passed into the long-running
// patchers so a phase that takes eleven seconds is not one jump.
//
// ⚠ CALLED PER FILE, NEVER PER BYTE. Each call reaches the GUI through
// `slint::invoke_from_event_loop`, so the cost is a cross-thread post; per file it
// is at most a hundred over a whole run, and per byte it would be a denial of
// service against the UI thread the bar lives on.
using StepProgress = std::function<void(double)>;

// The measured rates above, as functions so a call site reads as what it is doing
// rather than as an arithmetic expression.
double MsForObjScan(std::uint64_t bytes);
double MsForObjPatch(std::uint64_t bytes);
double MsForAcfAttach(std::uint64_t bytes);
double MsForAcfSpots(std::uint64_t bytes);
double MsForCopy(std::uint64_t bytes);

// Total bytes of the regular files in `dirUtf8` whose name ends in `ext`
// (lowercased comparison). ⚠ SIZES ONLY — this opens no file, which is what makes
// planning an 11-second phase cost under a millisecond.
//
// ⚠ MATCH THE WALK TO THE PHASE YOU ARE PRICING. `patch_glow` iterates one level;
// `patch_realwings` recurses. Pricing a recursive phase with a flat walk is how an
// estimate silently loses whatever sits in a subfolder.
std::uint64_t BytesOfFilesWithExt(const std::string& dirUtf8, const std::string& ext,
                                  bool recursive = false);

// Size of one file, or 0 if it is not there. Never throws: a plan is an estimate,
// and a missing file is a phase that will turn out to cost nothing.
std::uint64_t FileSizeOrZero(const std::string& pathUtf8);

// ⚠ HOW A STEP THE PLAN NEVER PRICED BECOMES VISIBLE. `Plan::Begin` ignores a name
// it does not know, deliberately — a bar that is slightly wrong beats an install
// that throws — and that swallows a whole class of mistake in SILENCE: a phase added
// to the run with a `Begin` and no matching `Add` simply never moves the bar.
//
// ⚠ IT IS NOT DETECTABLE FROM THE FRACTION, which is why this hook exists rather
// than a test on the numbers. An unpriced step is missing from the denominator as
// well as the numerator, so the run still ends on exactly 1.000 and every arithmetic
// check passes. Verified the hard way: the test written to catch this passed against
// a mutation that deleted a `plan.Add`.
//
// Null in production and installed by the test suite, which treats any call as a
// failure. ⚠ ONE GLOBAL, NOT THREAD-SAFE — set it before a run, not during one.
using UnknownStepHook = std::function<void(const std::string&)>;
void SetUnknownStepHook(UnknownStepHook hook);

// The shape of a run: every step it will take, with the estimated cost of each.
//
// ⚠ BUILD IT WHOLE BEFORE DOING ANY WORK. A plan appended to as the run proceeds
// has a growing denominator, so the bar goes backwards — which is the one thing a
// progress bar must never do, and it looks like a bug in whatever step was running
// when it happened.
class Plan {
public:
    // ⚠ EVERY STEP COSTS AT LEAST `kMsStepFloor`, WHATEVER ITS INPUTS SIZE TO. Two
    // reasons, and the second is the one that bit:
    //
    //  * It is true. A step that writes nothing still opens files, stats a folder
    //    and writes a log line. Nothing here costs literally zero.
    //  * Without it the whole plan degenerates on small inputs. Against the test
    //    fixture — where every payload file is a stub of a few dozen bytes — every
    //    size-derived step priced to under a thousandth of a millisecond, so the
    //    fraction was carried entirely by the two flat constants. A step could be
    //    dropped from the run and `Fraction()` still landed on 1.000: the guard in
    //    `install_tests.cpp` that exists to catch exactly that mistake passed
    //    against a deliberate mutation. A floor makes the arithmetic non-degenerate
    //    for any input, which is what makes that test able to fail.
    void Add(const std::string& name, double estimatedMs);

    // The fixed cost of being a step at all. See Add().
    static constexpr double kMsStepFloor = 1.0;

    bool Empty() const { return steps_.empty(); }
    std::size_t Size() const { return steps_.size(); }
    double TotalMs() const { return totalMs_; }
    const std::string& NameAt(std::size_t i) const { return steps_[i].name; }
    double MsAt(std::size_t i) const { return steps_[i].ms; }

    // Where the caller is now. `Begin` names the step being entered — ⚠ BY NAME,
    // NOT BY INDEX, because the run skips steps (no wing mod, no cockpit mod, no
    // glow map) and an index would silently point at the wrong one the first time
    // a branch is not taken. An unknown name is ignored rather than fatal: a bar
    // that is slightly wrong beats an install that throws.
    void Begin(const std::string& name);
    // Fraction 0..1 through the current step.
    void Within(double fraction);
    // The current step is done; its whole estimate is banked.
    void Finish();

    // 0..1 overall. ⚠ MONOTONIC BY CONSTRUCTION — `Within` only ever moves the
    // fraction of the CURRENT step, and `Finish` banks that step's full estimate,
    // so an under-estimated step stalls at its own ceiling rather than lending the
    // bar to the next one and snapping back.
    double Fraction() const;

private:
    struct Step {
        std::string name;
        double ms = 0.0;
    };

    std::vector<Step> steps_;
    double totalMs_ = 0.0;
    double doneMs_ = 0.0;        // banked, from finished steps
    double currentMs_ = 0.0;     // the running step's estimate
    double withinMs_ = 0.0;      // how much of it is reported done
    bool inStep_ = false;
};

}  // namespace progress
}  // namespace photon
