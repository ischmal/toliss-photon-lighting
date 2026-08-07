// ToLiss Photon Lighting — THE version definition.
//
// ⚠ THIS IS THE ONE PLACE. Everything else reads it: the plugin's About tab
// (`kPhotonVersion` used to be defined in plugin.cpp), the installer's OBJ marker
// and release-bundle name, `build/make_release.py`, `build/readme.py`.
//
// Why it earns its own header: the plugin's copy and the installer's copy DRIFTED
// (0.6 against 0.7) through a whole feature before anyone noticed, because nothing
// in-sim looks wrong when they disagree — the About tab simply reports an older
// version than the files beside it, which is exactly what a half-finished install
// looks like. `tests/test_version.py` pinned them equal afterwards; single-sourcing
// them is what makes the pin unnecessary.
//
// ⚠ THE PYTHON SIDE READS THIS FILE BY REGEX — `tests/test_version.py`,
// `build/make_release.py`, `build/readme.py` — so keep the definition below on ONE
// line, as a `constexpr char` array with a plain string literal. (Those readers
// skip comment lines, because the first one matched an example spelled out up here
// and reported the placeholder as the release.)
// `installer/constants.VERSION` is still a second copy during the transition and
// is pinned against this file by tests/test_version.py; it dies with the Python
// installer (docs/installer_cpp_plan.md §8c).
//
// Format: dotted numbers only. It reaches a filename and the OBJ marker line, so
// a stray space or a "v" prefix would land on disk.
#pragma once

namespace photon {

constexpr char kPhotonVersion[] = "0.8";

}  // namespace photon
