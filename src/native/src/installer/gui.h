// The GUI installer — the Slint front-end, and the third presentation layer over
// photoncore beside `cli.cpp` and `screens.cpp`.
//
// ⚠ NO INSTALL LOGIC LIVES HERE, exactly as none lives in the TUI. Detection, the
// actions, the manifest and every file patcher are photoncore's, and the three
// front-ends differ only in how they ask and how they report. A behavior that
// exists in one of them and not the others is a bug in that one — the CLI's
// `--json` output is the contract the GUI has to agree with.
//
// Built only when `-DPHOTON_GUI=ON`; see docs/installer_gui_plan.md and
// ui/README.md.
#pragma once

#include <string>

namespace photon {
namespace installer {

// Runs the whole graphical installer. Returns the process exit code.
//
// Same two arguments as `RunTui`, and they mean the same things:
// `xplaneRootOverride` skips detection (the `--xplane-root` argument), `dryRun`
// logs every action and performs no filesystem writes.
int RunGui(const std::string& xplaneRootOverride, bool dryRun);

}  // namespace installer
}  // namespace photon
