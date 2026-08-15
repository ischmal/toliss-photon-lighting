// The headless CLI — `photon-installer <subcommand> … --json`.
//
// New in the C++ port, and load-bearing for more than scripting:
//
//  * it is the layer the PYTHON INTEGRATION TESTS drive (they build fake trees
//    exactly as they always did, then call the binary through subprocess), and
//  * it is what the PARITY HARNESS runs, applying the Python installer and this
//    one to two copies of the same tree and diffing the results byte for byte.
//    That harness is the single strongest correctness net for the port, because
//    `actions.py`'s value is in accumulated edge-case handling and a tree diff
//    catches a missed branch that reading the code will not.
//
// ⚠ NO PROMPTS IN CLI MODE. Missing required input is an error, and the
// X-Plane-running guard becomes a hard refusal plus `--force` — a scripted run must
// never block on something nobody is there to answer.
#pragma once

#include <string>
#include <vector>

namespace photon {
namespace installer {

// argv[0] is expected at the front, as usual. Returns a process exit code.
int RunCli(const std::vector<std::string>& args);

// Whether these arguments name a subcommand at all. Bare `photon-installer` runs
// the TUI; anything else is CLI mode.
bool IsCliInvocation(const std::vector<std::string>& args);

std::string UsageText();

}  // namespace installer
}  // namespace photon
