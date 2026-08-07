// The one persisted installer preference: the last manually-confirmed X-Plane
// root, so a returning user is not asked to retype or re-browse it.
//
// Lives in a per-OS USER config directory, independent of any X-Plane install, so
// it survives an uninstall/reinstall of the aircraft mod and of the installer
// itself. Deliberately separate from `actions` (whose writes target the X-Plane
// install being modified) and from `detect` (which stays pure filesystem probing).
//
//   Windows   %APPDATA%\ToLissPhoton\installer_config.json
//   macOS     ~/Library/Application Support/ToLissPhoton/…
//   Linux     $XDG_CONFIG_HOME or ~/.config, then ToLissPhoton/…
//
// ⚠ CALLERS MUST STILL REVALIDATE what comes back. The remembered root may have
// been moved, renamed or deleted since; a saved path is a hint, never a fact.
#pragma once

#include <string>

namespace photon {
namespace config {

std::string ConfigDir();
std::string ConfigPath();

// Empty when never saved, unreadable, or corrupt — all three are the same thing
// to a caller, and none of them is worth an error.
std::string LoadSavedRoot();

// Best-effort. A config directory we cannot write is not a reason to fail an
// install the user is otherwise able to complete.
void SaveRoot(const std::string& rootUtf8);
void ClearSavedRoot();

}  // namespace config
}  // namespace photon
