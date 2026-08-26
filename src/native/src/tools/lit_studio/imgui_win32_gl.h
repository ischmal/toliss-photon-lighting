// A minimal Win32 + legacy-OpenGL platform/renderer backend for Dear ImGui.
//
// The repository vendors Dear ImGui WITHOUT its `backends/` folder — the plugin
// draws through its own X-Plane backend (`imgui_xplm.cpp`), which is bound to an
// XPLM window and a sim-owned GL context and cannot be reused off-sim. This is
// the desktop equivalent, kept deliberately small: one window, GL 1.1
// fixed-function, no GLFW, no GLEW, no loader. Everything it calls has been in
// `opengl32.dll` since 1996, which is what lets the tool build with nothing on
// the machine but the Windows SDK.
//
// ⚠ It follows imgui_xplm.cpp's two hard-won rules, which are not this file's to
// rediscover:
//   * ITERATE `ImDrawData::CmdLists`, NEVER `CmdListsCount` — the latter is
//     obsoleted in 1.92.9 and reads 0 on frames that have draw lists, so a loop
//     written against it compiles and silently draws nothing.
//   * Do NOT set `ImGuiBackendFlags_RendererHasTextures`. That opts into 1.92's
//     texture-request protocol; the legacy `GetTexDataAsRGBA32` + `SetTexID`
//     path below is what the plugin uses and what this mirrors.
#pragma once

#include <cstdint>
#include <string>

#include "imgui.h"

namespace photon {
namespace studio {

// An opaque GL texture name, handed to ImGui as an ImTextureID.
using TextureId = unsigned int;

class Window {
public:
    ~Window();

    // Opens the window and makes its GL context current. False on any failure,
    // with `error` set.
    bool Create(const char* title, int width, int height, std::string& error);

    // Drains the message queue. Returns false once the window has been closed.
    bool Pump();

    // Begins an ImGui frame sized to the client area.
    void BeginFrame();

    // Ends the frame, draws it and presents. `clear` is the backdrop color.
    void EndFrame(const float clear[3]);

    void Destroy();

    int Width() const { return width_; }
    int Height() const { return height_; }

    // --- textures ------------------------------------------------------------
    // RGBA8, tightly packed. Create returns 0 on failure.
    static TextureId CreateTexture(int w, int h, const std::uint8_t* rgba);
    static void UpdateTexture(TextureId id, int w, int h,
                              const std::uint8_t* rgba);
    static void DestroyTexture(TextureId id);
    // The largest square texture this GL will accept, for deciding whether a
    // 4096-pixel atlas can be shown at full size.
    static int MaxTextureSize();

private:
    void* hwnd_ = nullptr;
    void* dc_ = nullptr;
    void* rc_ = nullptr;
    unsigned int fontTex_ = 0;
    int width_ = 0;
    int height_ = 0;
    bool closed_ = false;
    double lastTime_ = 0.0;
};

}  // namespace studio
}  // namespace photon
