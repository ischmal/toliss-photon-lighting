// Dear ImGui inside an X-Plane floating window.
//
// One ImguiWindow == one XPLMCreateWindowEx window with its own ImGui context.
// Give it a BuildUi callback and it runs the frame; the callback only ever calls
// ImGui:: functions, and knows nothing about X-Plane.
//
//     gWin = new ImguiWindow(900, 600, "ToLiss Photon - Panel FX");
//     gWin->BuildUi = [] { ImGui::Begin("Layers"); ...; ImGui::End(); };
//     gWin->SetVisible(true);
//
// See imgui_xplm.cpp for why the stock imgui_impl_opengl2 backend is not used.
#pragma once

#include <functional>

#include "XPLMDisplay.h"

#include "imgui.h"

class ImguiWindow {
public:
    // Size is in boxels. The window is created hidden and centred on the screen.
    ImguiWindow(int width, int height, const char* title);
    ~ImguiWindow();

    ImguiWindow(const ImguiWindow&)            = delete;
    ImguiWindow& operator=(const ImguiWindow&) = delete;

    // Called between NewFrame and Render, with this window's context current.
    std::function<void()> BuildUi;

    void SetVisible(bool visible);
    bool IsVisible() const;
    void BringToFront();

    XPLMWindowID Handle() const { return mWin; }
    // False if the window or the ImGui context could not be created. Every entry
    // point checks this, so a failed construction degrades to "no window" rather
    // than to a crash on first draw.
    bool Ok() const { return mWin != nullptr && mCtx != nullptr; }

private:
    // The boxel -> framebuffer-pixel transform in force for this draw. Only
    // glScissor needs it; vertices ride X-Plane's own projection unchanged.
    // See imgui_xplm.cpp for why it does not come from glGet.
    struct Transform {
        float mv[16];
        float pr[16];
        int   vp[4];      // GL's form: x, y, width, height
    };

    void Draw();
    void RenderDrawData(ImDrawData* data);
    void FeedMousePos(int globalX, int globalY);
    bool BuildFontTexture();

    static bool ReadTransform(Transform* t);
    void DrawDiagnosticMarkers();
    // Maps this window's own corners through 't'. False if the result is not a
    // sane pixel rectangle, which is the backend's signal that the transform is
    // unusable and clipping has to be skipped rather than trusted.
    bool WindowPixelRect(const Transform& t, float out[4]) const;
    void LogFirstFrameDiagnostics(const Transform& t, const ImDrawData* data,
                                  bool clipOk, const float winPx[4]);

    static void DrawCb(XPLMWindowID, void* ref);
    static int  MouseCb(XPLMWindowID, int x, int y, int status, void* ref);
    static int  RightClickCb(XPLMWindowID, int x, int y, int status, void* ref);
    static int  WheelCb(XPLMWindowID, int x, int y, int wheel, int clicks, void* ref);
    static void KeyCb(XPLMWindowID, char key, XPLMKeyFlags flags, char vkey,
                      void* ref, int losingFocus);
    static XPLMCursorStatus CursorCb(XPLMWindowID, int x, int y, void* ref);

    XPLMWindowID  mWin      = nullptr;
    ImGuiContext* mCtx      = nullptr;
    int           mFontTex  = 0;
    bool          mFontTried = false;
    float         mLastTime = 0.0f;
    // Window geometry in boxels, refreshed every draw. ImGui works in
    // window-local coordinates; these convert to and from X-Plane's global ones.
    int mLeft = 0, mTop = 0, mRight = 0, mBottom = 0;
    bool mHadMouseDown = false;
    // One diagnostic dump per window per session. A blank tool window gives no
    // clue on its own — this is the difference between one log read and a
    // build-deploy-restart cycle per hypothesis.
    bool mLoggedDiag = false;
    bool mLoggedEntry = false;
};

namespace PhotonImgui {
// The backend has no access to plugin.cpp's Log(); this hands it one. Safe to
// leave unset — messages are then dropped rather than crashing.
void SetLogger(void (*fn)(const char*));
void Log(const char* msg);
void AssertFailed(const char* expr, const char* file, int line);

// Diagnostic mode. Logs the first frame of every window in detail and paints two
// GL sanity markers in the bottom-left of the client area:
//
//   * an UNTEXTURED magenta square  — did any immediate-mode GL of ours survive
//     to the screen in a window draw callback at all?
//   * a TEXTURED square showing the font atlas — did the atlas upload and does
//     texturing work?
//
// They are deliberately outside the scissor and outside ImGui's geometry, so
// what they show is independent of everything the UI depends on. Which of the
// three (neither / magenta only / both) appears splits a blank window into three
// disjoint causes in one sim start, which is otherwise a restart per hypothesis.
void SetDiagnostics(bool on);
bool Diagnostics();
}
