// Dear ImGui build configuration for ToLiss Photon.
//
// Reached via -DIMGUI_USER_CONFIG="photon_imconfig.h" from CMakeLists.txt, which
// imgui.h includes BEFORE the stock imconfig.h. Keeping our settings here means
// third_party/imgui/ stays byte-identical to upstream and a version bump is a
// straight file copy.
#pragma once

namespace PhotonImgui {
// Defined in imgui_xplm.cpp. Logs and returns — see below.
void AssertFailed(const char* expr, const char* file, int line);
}

// ⚠ An ImGui assertion must never abort.
//
// The default IM_ASSERT is <cassert>'s assert(), which on a failure calls
// abort() — inside X-Plane that is not a diagnostic, it is the whole simulator
// disappearing with no message, during what is by definition a dev session. So
// a failed assertion logs to Log.txt and execution continues.
//
// Continuing past a violated invariant is not free, and the point is not that
// it is safe: it is that a garbled tool window you can screenshot beats a dead
// sim you have to relaunch. The logged file:line is the real output.
//
// Written in expression form rather than do/while(0) because ImGui uses
// IM_ASSERT inside expressions and one-line inline functions.
#define IM_ASSERT(_EXPR) \
    ((void)((_EXPR) ? 0 : (PhotonImgui::AssertFailed(#_EXPR, __FILE__, __LINE__), 0)))

// No .ini file is wanted (we persist our own state, and X-Plane's working
// directory is the sim root — ImGui would drop imgui.ini there). The backend
// also sets io.IniFilename = nullptr; this just removes the machinery.
#define IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS
#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS

// The default file functions pull in stdio; we never load fonts or images from
// disk, so this drops the dependency without losing anything used.
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
