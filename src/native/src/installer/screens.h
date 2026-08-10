// The interactive installer: six screens and the state machine that walks them.
//
// A port of `install.py`'s flow (docs/installer_cpp_plan.md §4). The topology is
// the contract, not an implementation detail:
//
//   launch → X-Plane root → aircraft → action → (guard) → perform → complete
//
// ⚠ ESC STEPS BACK EXACTLY ONE STAGE, with two deliberate exceptions:
//   * aircraft-selection → root, because a different root has different aircraft
//     under it and re-choosing the aircraft alone would be a dead end;
//   * the Complete screen has NO back at all — the install it reports on already
//     happened, so there is no previous step to rewind to.
//
// Every screen throws `tui::Back` on ESC and the flow catches it at exactly one
// level, which is what keeps that topology readable in one function.
#pragma once

#include <string>
#include <vector>

namespace photon {
namespace installer {

// Runs the whole interactive installer. Returns the process exit code.
//
// `xplaneRootOverride` skips the root screen (the `--xplane-root` argument);
// `dryRun` logs every action and performs no filesystem writes.
int RunTui(const std::string& xplaneRootOverride, bool dryRun);

}  // namespace installer
}  // namespace photon
