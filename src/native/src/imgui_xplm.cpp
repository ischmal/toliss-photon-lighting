// Dear ImGui backend for X-Plane. See imgui_xplm.h for the interface.
//
// ============================================================================
// Why not imgui_impl_opengl2 + a platform backend
// ============================================================================
//
// The stock backends assume they own the window, the GL context and the event
// loop. Inside X-Plane a plugin owns none of those, and three things in
// particular have to be done X-Plane's way:
//
//  * TEXTURES COME FROM XPLMGenerateTextureNumbers, not glGenTextures, and are
//    bound with XPLMBindTexture2d. X-Plane caches texture-binding state; a raw
//    glBindTexture desynchronises that cache and the corruption shows up in
//    unrelated rendering later in the frame, which is a miserable thing to
//    debug.
//  * THERE IS NO EVENT LOOP. Input arrives as XPLM window callbacks, and the
//    frame is driven by X-Plane calling our draw callback.
//  * THE PROJECTION IS ALREADY SET UP, in global boxel coordinates — the same
//    space XPLMDrawString takes. So vertices need no projection of our own,
//    only a translate/flip into the window. glScissor is the exception: it is
//    in framebuffer PIXELS, so clip rectangles are mapped through the
//    modelview/projection/viewport X-Plane PUBLISHES AS DATAREFS rather than
//    through glGet or an assumed DPI scale. See ReadTransform — getting that
//    wrong is invisible in the vertices and fatal to the scissor box, which is
//    to say it blanks the whole window while reporting no GL error.
//
// ⚠ RESTORE EVERY STATE TOGGLE. Same rule as the panel probe, for the same
// reason: this draws mid-frame inside X-Plane's own rendering, and a leaked
// scissor box or blend func corrupts whatever draws next. Not hypothetical —
// it has already cost this project a debugging round.
//
// Geometry goes out in immediate mode, one glBegin(GL_TRIANGLES) per draw
// command. A tool window is a few thousand vertices a frame, the sim's GL
// bridge handles that without noticing, and it avoids client-array state
// (glVertexPointer & co.) that would then need saving and restoring too. If a
// UI ever gets heavy enough for this to matter, that is the moment to switch to
// buffers — not before.

#include "imgui_xplm.h"

#include <cstdio>
#include <cstring>

#include "XPLMDataAccess.h"   // the boxel->pixel transform, see ReadTransform
#include "XPLMGraphics.h"
#include "XPLMProcessing.h"   // XPLMGetElapsedTime, for io.DeltaTime
#include "XPLMUtilities.h"

#if IBM
  #include <windows.h>
  #include <GL/gl.h>
#elif APL
  #include <OpenGL/gl.h>
#else
  #include <GL/gl.h>
#endif

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

namespace PhotonImgui {

static void (*gLogger)(const char*) = nullptr;
static bool gDiagnostics = false;

void SetLogger(void (*fn)(const char*)) { gLogger = fn; }

void SetDiagnostics(bool on) { gDiagnostics = on; }
bool Diagnostics() { return gDiagnostics; }

void Log(const char* msg) {
    if (gLogger) gLogger(msg);
}

void AssertFailed(const char* expr, const char* file, int line) {
    // Once per site would need a registry; once per session is enough to stop a
    // per-frame assertion from filling Log.txt with megabytes.
    static bool reported = false;
    if (reported) return;
    reported = true;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "imgui: ASSERTION FAILED '%s' at %s:%d — continuing anyway "
                  "(further assertions this session are not logged). The UI may "
                  "misbehave from here; restart the sim before trusting it.",
                  expr, file, line);
    Log(buf);
}

}  // namespace PhotonImgui

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// The UI is authored at ImGui's native scale. Raise this if the tool windows
// read small on a high-density display — it scales the font and every style
// metric together, which is the only way to scale that does not leave padding
// and text disagreeing.
static const float kUiScale = 1.0f;

namespace {

// RAII context switch. Every entry point from X-Plane needs one: ImGui keeps a
// single global "current context" and each window here owns its own, so a
// callback that forgets this would build its frame into whichever window last
// drew.
class ScopedContext {
public:
    explicit ScopedContext(ImGuiContext* ctx) : mPrev(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(ctx);
    }
    ~ScopedContext() { ImGui::SetCurrentContext(mPrev); }
    ScopedContext(const ScopedContext&)            = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
private:
    ImGuiContext* mPrev;
};

// Datarefs carrying the transform. Resolved on first use rather than in an init
// call the owner would have to remember to make.
XPLMDataRef gMvDref = nullptr, gPrDref = nullptr, gVpDref = nullptr;
bool        gDrefsResolved = false;

void ResolveTransformDrefs() {
    if (gDrefsResolved) return;
    gDrefsResolved = true;
    gMvDref = XPLMFindDataRef("sim/graphics/view/modelview_matrix");
    gPrDref = XPLMFindDataRef("sim/graphics/view/projection_matrix");
    gVpDref = XPLMFindDataRef("sim/graphics/view/viewport");
}

// Map a point in X-Plane's global boxel space to framebuffer pixels.
void MapPoint(const float mv[16], const float pr[16], const int vp[4],
              float bx, float by, float* outX, float* outY) {
    const float ex = mv[0] * bx + mv[4] * by + mv[12];
    const float ey = mv[1] * bx + mv[5] * by + mv[13];
    const float ez = mv[2] * bx + mv[6] * by + mv[14];
    const float ew = mv[3] * bx + mv[7] * by + mv[15];

    const float cx = pr[0] * ex + pr[4] * ey + pr[8]  * ez + pr[12] * ew;
    const float cy = pr[1] * ex + pr[5] * ey + pr[9]  * ez + pr[13] * ew;
    float       cw = pr[3] * ex + pr[7] * ey + pr[11] * ez + pr[15] * ew;
    if (cw == 0.0f) cw = 1.0f;

    *outX = (float)vp[0] + (float)vp[2] * (cx / cw * 0.5f + 0.5f);
    *outY = (float)vp[1] + (float)vp[3] * (cy / cw * 0.5f + 0.5f);
}

bool Finite(float v) { return v == v && v > -1.0e9f && v < 1.0e9f; }

ImGuiKey VirtualKeyToImGui(unsigned char vk) {
    switch (vk) {
        case XPLM_VK_TAB:    return ImGuiKey_Tab;
        case XPLM_VK_LEFT:   return ImGuiKey_LeftArrow;
        case XPLM_VK_RIGHT:  return ImGuiKey_RightArrow;
        case XPLM_VK_UP:     return ImGuiKey_UpArrow;
        case XPLM_VK_DOWN:   return ImGuiKey_DownArrow;
        case XPLM_VK_PRIOR:  return ImGuiKey_PageUp;
        case XPLM_VK_NEXT:   return ImGuiKey_PageDown;
        case XPLM_VK_HOME:   return ImGuiKey_Home;
        case XPLM_VK_END:    return ImGuiKey_End;
        case XPLM_VK_DELETE: return ImGuiKey_Delete;
        case XPLM_VK_BACK:   return ImGuiKey_Backspace;
        case XPLM_VK_SPACE:  return ImGuiKey_Space;
        case XPLM_VK_ESCAPE: return ImGuiKey_Escape;
        case XPLM_VK_RETURN:
        case XPLM_VK_ENTER:
        case XPLM_VK_NUMPAD_ENT: return ImGuiKey_Enter;
        default: break;
    }
    // The letter and digit blocks are contiguous and equal to ASCII, so the
    // clipboard shortcuts (ctrl-A/C/V/X/Y/Z) and numeric entry work without
    // enumerating 36 cases.
    if (vk >= XPLM_VK_A && vk <= XPLM_VK_Z)
        return (ImGuiKey)(ImGuiKey_A + (vk - XPLM_VK_A));
    if (vk >= XPLM_VK_0 && vk <= XPLM_VK_9)
        return (ImGuiKey)(ImGuiKey_0 + (vk - XPLM_VK_0));
    return ImGuiKey_None;
}

}  // namespace

// ---------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------

ImguiWindow::ImguiWindow(int width, int height, const char* title) {
    mCtx = ImGui::CreateContext();
    if (!mCtx) {
        PhotonImgui::Log("imgui: CreateContext failed — no window");
        return;
    }
    ScopedContext scoped(mCtx);

    ImGuiIO& io = ImGui::GetIO();
    // No imgui.ini: X-Plane's working directory is the sim root, and this
    // project persists its own state in Output/preferences anyway.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Deliberately NOT setting ImGuiBackendFlags_RendererHasTextures: that opts
    // into ImGui 1.92's incremental texture path, and ours is a single static
    // font atlas uploaded once through XPLMGenerateTextureNumbers.
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    io.BackendPlatformName = "photon-xplm";
    io.BackendRendererName = "photon-gl-immediate";

    ImGui::StyleColorsDark();
    if (kUiScale != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(kUiScale);
        io.FontGlobalScale = kUiScale;
    }

    // ⚠ The font atlas is NOT uploaded here. glTexImage2D needs a current GL
    // context, and a window may be constructed from XPluginStart or a menu
    // handler, neither of which is guaranteed to have one. Draw() does it on the
    // first frame instead, where a context provably exists.

    // Centre on the main screen. Boxels, and the origin is bottom-left.
    int sl = 0, st = 0, sr = 0, sb = 0;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    const int cx = (sl + sr) / 2, cy = (sb + st) / 2;

    XPLMCreateWindow_t p = {};
    p.structSize              = sizeof(p);
    p.left                    = cx - width / 2;
    p.right                   = cx + width / 2;
    p.top                     = cy + height / 2;
    p.bottom                  = cy - height / 2;
    p.visible                 = 0;
    p.refcon                  = this;
    p.drawWindowFunc          = DrawCb;
    p.handleMouseClickFunc    = MouseCb;
    p.handleRightClickFunc    = RightClickCb;
    p.handleMouseWheelFunc    = WheelCb;
    p.handleKeyFunc           = KeyCb;
    p.handleCursorFunc        = CursorCb;
    p.layer                   = xplm_WindowLayerFloatingWindows;
    // X-Plane draws the frame, title bar, close button, and handles dragging
    // and resizing. ImGui then only has to fill the client area, which is a lot
    // less code than self-decorating and syncing geometry back — and it makes
    // these windows behave like every other X-Plane window.
    p.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;

    mWin = XPLMCreateWindowEx(&p);
    if (!mWin) {
        PhotonImgui::Log("imgui: XPLMCreateWindowEx failed");
        return;
    }
    XPLMSetWindowTitle(mWin, title);
    XPLMSetWindowResizingLimits(mWin, 400, 260, 4096, 4096);
    XPLMSetWindowPositioningMode(mWin, xplm_WindowPositionFree, -1);

    if (PhotonImgui::Diagnostics()) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "imgui: created '%s' at boxels l=%d b=%d r=%d t=%d",
                      title, p.left, p.bottom, p.right, p.top);
        PhotonImgui::Log(buf);
    }
}

ImguiWindow::~ImguiWindow() {
    if (mWin) XPLMDestroyWindow(mWin);
    // The font texture is not deleted: X-Plane hands out texture numbers from
    // its own pool and offers no matching release call, and these windows are
    // created once per session anyway.
    if (mCtx) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::DestroyContext(mCtx);
        // DestroyContext clears the current context if it was the one destroyed.
        if (prev != mCtx) ImGui::SetCurrentContext(prev);
        mCtx = nullptr;
    }
}

bool ImguiWindow::BuildFontTexture() {
    ImGuiIO& io = ImGui::GetIO();

    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    if (!pixels || w <= 0 || h <= 0) return false;

    XPLMGenerateTextureNumbers(&mFontTex, 1);
    XPLMBindTexture2d(mFontTex, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    while (glGetError() != GL_NO_ERROR) {}   // clear anything inherited
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    // ⚠ Worth a log line of its own. EVERY ImGui vertex is textured — solid
    // rectangles sample the atlas's white pixel — so a failed upload does not
    // cost the text, it costs the entire UI, and it does it by making everything
    // transparent rather than by raising anything.
    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "imgui: font atlas upload FAILED — glTexImage2D(%dx%d) -> "
                      "glGetError 0x%x. The whole UI will draw transparent.",
                      w, h, (unsigned)err);
        PhotonImgui::Log(buf);
        return false;
    }

    io.Fonts->SetTexID((ImTextureID)(intptr_t)mFontTex);
    if (PhotonImgui::Diagnostics()) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "imgui: font atlas uploaded — %dx%d RGBA, X-Plane texture %d",
                      w, h, mFontTex);
        PhotonImgui::Log(buf);
    }
    return true;
}

// ---------------------------------------------------------------------------
// visibility
// ---------------------------------------------------------------------------

void ImguiWindow::SetVisible(bool visible) {
    if (!Ok()) return;
    XPLMSetWindowIsVisible(mWin, visible ? 1 : 0);
    if (!visible && XPLMHasKeyboardFocus(mWin)) XPLMTakeKeyboardFocus(nullptr);
}

bool ImguiWindow::IsVisible() const {
    return Ok() && XPLMGetWindowIsVisible(mWin) != 0;
}

void ImguiWindow::BringToFront() {
    if (!Ok()) return;
    XPLMSetWindowIsVisible(mWin, 1);
    XPLMBringWindowToFront(mWin);
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

void ImguiWindow::DrawCb(XPLMWindowID, void* ref) {
    static_cast<ImguiWindow*>(ref)->Draw();
}

void ImguiWindow::Draw() {
    if (!Ok()) return;
    ScopedContext scoped(mCtx);
    ImGuiIO& io = ImGui::GetIO();

    // ⚠ Logged FIRST, before anything that can return early. "Is our draw
    // callback being called at all" is the question every other diagnostic here
    // assumes the answer to, and a diagnostic placed after an early return
    // reports nothing in exactly the case you needed it.
    if (!mLoggedEntry) {
        mLoggedEntry = true;
        PhotonImgui::Log("imgui: draw callback entered — the window is being asked to draw");
    }

    // First frame with a live GL context — see the constructor. Attempted once:
    // a per-frame retry would fill Log.txt at the frame rate, and nothing about
    // the next frame makes an atlas that would not build build.
    if (!mFontTried) {
        mFontTried = true;
        if (!BuildFontTexture())
            PhotonImgui::Log("imgui: font atlas unavailable — the UI will draw "
                             "transparent, i.e. as an empty window");
    }

    XPLMGetWindowGeometry(mWin, &mLeft, &mTop, &mRight, &mBottom);
    const float w = (float)(mRight - mLeft), h = (float)(mTop - mBottom);
    if (w < 1.0f || h < 1.0f) return;
    io.DisplaySize = ImVec2(w, h);

    const float now = XPLMGetElapsedTime();
    // ImGui asserts on a non-positive delta, and the first frame has no
    // previous timestamp to subtract.
    io.DeltaTime = (mLastTime > 0.0f && now > mLastTime) ? (now - mLastTime) : (1.0f / 60.0f);
    mLastTime = now;

    int mx = 0, my = 0;
    XPLMGetMouseLocationGlobal(&mx, &my);
    FeedMousePos(mx, my);

    ImGui::NewFrame();

    // One ImGui window filling the client area. X-Plane already drew the frame
    // and title bar, so this one carries no chrome of its own — the alternative
    // is two nested title bars, one of which does not respond to dragging.
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                 | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                                 | ImGuiWindowFlags_NoBringToFrontOnFocus
                                 | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##photon_root", nullptr, flags)) {
        if (BuildUi) BuildUi();
    }
    ImGui::End();

    ImGui::Render();
    RenderDrawData(ImGui::GetDrawData());
}

// ⚠ THE TRANSFORM COMES FROM DATAREFS, NOT FROM glGet.
//
// Under the Vulkan renderer our drawing rides a GL bridge, and the
// fixed-function matrix state glGetFloatv reports is not the transform actually
// in force for a window draw callback. X-Plane publishes the real one, which is
// why these three datarefs exist at all. Reading it from GL instead is what made
// the first cut of this backend draw a blank window: the vertices were fine —
// they ride X-Plane's own projection and never touch this — but every clip rect
// mapped to nonsense, and a nonsense scissor box discards the entire UI while
// reporting no GL error.
//
// glGet stays as the fallback: on an older sim, or a GL-renderer one, it is
// right, and the datarefs may not exist.
bool ImguiWindow::ReadTransform(Transform* t) {
    ResolveTransformDrefs();

    if (!gMvDref || XPLMGetDatavf(gMvDref, t->mv, 0, 16) != 16)
        glGetFloatv(GL_MODELVIEW_MATRIX, t->mv);
    if (!gPrDref || XPLMGetDatavf(gPrDref, t->pr, 0, 16) != 16)
        glGetFloatv(GL_PROJECTION_MATRIX, t->pr);

    // ⚠ sim/graphics/view/viewport is left, bottom, RIGHT, TOP — corners, not a
    // size. GL's is x, y, width, height. Mixing the two up costs nothing while
    // the viewport starts at the origin and silently mis-scales as soon as it
    // does not.
    int corners[4] = {0, 0, 0, 0};
    t->vp[0] = t->vp[1] = t->vp[2] = t->vp[3] = 0;
    if (gVpDref && XPLMGetDatavi(gVpDref, corners, 0, 4) == 4) {
        t->vp[0] = corners[0];
        t->vp[1] = corners[1];
        t->vp[2] = corners[2] - corners[0];
        t->vp[3] = corners[3] - corners[1];
    }
    if (t->vp[2] <= 0 || t->vp[3] <= 0) glGetIntegerv(GL_VIEWPORT, t->vp);

    return t->vp[2] > 0 && t->vp[3] > 0;
}

bool ImguiWindow::WindowPixelRect(const Transform& t, float out[4]) const {
    float ax, ay, bx, by;
    MapPoint(t.mv, t.pr, t.vp, (float)mLeft,  (float)mBottom, &ax, &ay);
    MapPoint(t.mv, t.pr, t.vp, (float)mRight, (float)mTop,    &bx, &by);
    if (!Finite(ax) || !Finite(ay) || !Finite(bx) || !Finite(by)) return false;

    // ⚠ Normalise rather than assume an ordering. Whether boxel-y-up comes out
    // pixel-y-up depends on the projection, and a negative width or height is a
    // GL error that silently drops every draw.
    out[0] = ax < bx ? ax : bx;
    out[1] = ay < by ? ay : by;
    out[2] = ax < bx ? bx : ax;
    out[3] = ay < by ? by : ay;

    // A window that maps to less than a pixel, or to somewhere the framebuffer
    // is not, means the transform is not the one being drawn through.
    if (out[2] - out[0] < 1.0f || out[3] - out[1] < 1.0f) return false;
    if (out[2] < (float)t.vp[0] || out[0] > (float)(t.vp[0] + t.vp[2])) return false;
    if (out[3] < (float)t.vp[1] || out[1] > (float)(t.vp[1] + t.vp[3])) return false;
    return true;
}

void ImguiWindow::LogFirstFrameDiagnostics(const Transform& t, const ImDrawData* data,
                                           bool clipOk, const float winPx[4]) {
    int vtx = 0, cmds = 0;
    for (int n = 0; n < data->CmdLists.Size; ++n) {
        vtx  += data->CmdLists[n]->VtxBuffer.Size;
        cmds += data->CmdLists[n]->CmdBuffer.Size;
    }
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* px = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);

    char buf[768];
    std::snprintf(buf, sizeof(buf),
                  "imgui: first frame — window boxels l=%d b=%d r=%d t=%d, "
                  "display %.0fx%.0f, font tex=%d atlas %dx%d, "
                  "%d list(s) %d cmd(s) %d vtx; viewport %d,%d %dx%d "
                  "(from %s); clip %s, window pixels %.0f,%.0f..%.0f,%.0f; "
                  "mv diag %.4f %.4f, proj diag %.6f %.6f tx,ty %.3f %.3f",
                  mLeft, mBottom, mRight, mTop, io.DisplaySize.x, io.DisplaySize.y,
                  mFontTex, fw, fh, data->CmdLists.Size, cmds, vtx,
                  t.vp[0], t.vp[1], t.vp[2], t.vp[3],
                  gVpDref ? "dataref" : "glGet",
                  clipOk ? "OK" : "UNUSABLE (scissor disabled for the frame)",
                  winPx[0], winPx[1], winPx[2], winPx[3],
                  t.mv[0], t.mv[5], t.pr[0], t.pr[5], t.pr[12], t.pr[13]);
    PhotonImgui::Log(buf);
}

// Two squares in the bottom-left of the client area, drawn in GLOBAL BOXELS with
// no scissor and no matrix push of ours — i.e. sharing as little as possible
// with the UI path, so what they show is evidence about that path rather than a
// second instance of the same failure.
//
//   magenta (left)  untextured    — immediate-mode GL from a window draw
//                                   callback reaches the screen
//   font atlas (right, white)     — the atlas uploaded and texturing works
//
// Neither visible / magenta only / both are three disjoint diagnoses. See the
// header for the reasoning.
//
// Drawn ONLY when there is no UI geometry to draw, so they never clutter a
// working window — their appearance is itself the signal that something is
// wrong, and they then say how far down the pipe it went.
void ImguiWindow::DrawDiagnosticMarkers() {
    const float sz = 40.0f;
    const float x0 = (float)mLeft + 4.0f;
    const float y0 = (float)mBottom + 4.0f;

    const GLboolean hadScissor = glIsEnabled(GL_SCISSOR_TEST);
    if (hadScissor) glDisable(GL_SCISSOR_TEST);
    const GLboolean hadCull = glIsEnabled(GL_CULL_FACE);
    if (hadCull) glDisable(GL_CULL_FACE);

    // Untextured: no texture units at all, so nothing about the atlas can
    // suppress it.
    XPLMSetGraphicsState(0, 0, 0, 0, 1, 0, 0);
    glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(x0,      y0);
    glVertex2f(x0 + sz, y0);
    glVertex2f(x0 + sz, y0 + sz);
    glVertex2f(x0,      y0 + sz);
    glEnd();

    if (mFontTex != 0) {
        const float x1 = x0 + sz + 4.0f;
        XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);
        XPLMBindTexture2d(mFontTex, 0);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x1,      y0);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x1 + sz, y0);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x1 + sz, y0 + sz);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x1,      y0 + sz);
        glEnd();
    }

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    if (hadCull) glEnable(GL_CULL_FACE);
    if (hadScissor) glEnable(GL_SCISSOR_TEST);
}

// ⚠ ITERATE data->CmdLists, NEVER data->CmdListsCount.
//
// That counter is obsolete as of 1.92.9 and is no longer maintained by the
// renderer: ImGui's own path pushes lists through AddDrawListToDrawDataEx, and
// only the public AddDrawList() helper — which nothing here calls — updates the
// count. So it reads 0 for every frame ImGui renders, while CmdLists is full and
// TotalVtxCount is in the hundreds.
//
// Writing the loop against it cost this backend a day: it compiles, it is what
// every pre-1.92 example and most of the internet still shows, and it fails by
// silently drawing NOTHING. If a version bump ever makes the UI vanish again,
// look here first.
void ImguiWindow::RenderDrawData(ImDrawData* data) {
    if (!data || data->CmdLists.Size <= 0) {
        if (!mLoggedDiag) {
            mLoggedDiag = true;
            PhotonImgui::Log("imgui: ImGui::Render produced NO draw lists — the "
                             "backend never had anything to draw. Look at the UI "
                             "build callback and the font, not at the GL path.");
        }
        // Still worth the sanity markers: if they appear, GL and the window are
        // both fine and the fault is entirely upstream of this function.
        if (PhotonImgui::Diagnostics()) DrawDiagnosticMarkers();
        return;
    }

    // The transform that maps global boxels to the framebuffer. Captured BEFORE
    // our own push, because that is the space clip rectangles have to be
    // expressed in to become a pixel scissor box.
    Transform xf;
    const bool haveXf = ReadTransform(&xf);

    // ⚠ Clip only when the transform demonstrably works. A wrong scissor box
    // discards the whole UI and looks exactly like "ImGui is broken"; drawing
    // unclipped merely lets a scrolled child overflow its pane. Degrade toward
    // the visible failure, not the invisible one.
    float winPx[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const bool clipOk = haveXf && WindowPixelRect(xf, winPx);

    if (!mLoggedDiag) {
        mLoggedDiag = true;
        LogFirstFrameDiagnostics(xf, data, clipOk, winPx);
    }

    // One texture unit, blended, no depth. XPLMSetGraphicsState is how X-Plane
    // wants this said — it keeps the sim's own shadow of the GL state correct.
    XPLMSetGraphicsState(0, 1, 0, 0, 1, 0, 0);

    GLint srcRGB = GL_SRC_ALPHA, dstRGB = GL_ONE_MINUS_SRC_ALPHA;
    glGetIntegerv(GL_BLEND_SRC, &srcRGB);
    glGetIntegerv(GL_BLEND_DST, &dstRGB);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const GLboolean hadScissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint oldScissor[4];
    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    if (clipOk) glEnable(GL_SCISSOR_TEST);
    else if (hadScissor) glDisable(GL_SCISSOR_TEST);

    // ImGui works top-left-down in window-local coordinates; X-Plane works
    // bottom-left-up in global ones. Translate to the window's top-left corner
    // and flip y, and every vertex then goes out untouched.
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glTranslatef((GLfloat)mLeft, (GLfloat)mTop, 0.0f);
    glScalef(1.0f, -1.0f, 1.0f);

    // ⚠ That y flip reverses triangle winding, so back-face culling would eat
    // the entire UI. Turned off here and restored below rather than assumed off.
    const GLboolean hadCull = glIsEnabled(GL_CULL_FACE);
    if (hadCull) glDisable(GL_CULL_FACE);

    for (int n = 0; n < data->CmdLists.Size; ++n) {
        const ImDrawList* cmds = data->CmdLists[n];
        const ImDrawVert* vtx  = cmds->VtxBuffer.Data;
        const ImDrawIdx*  idx  = cmds->IdxBuffer.Data;

        for (int c = 0; c < cmds->CmdBuffer.Size; ++c) {
            const ImDrawCmd& cmd = cmds->CmdBuffer[c];
            if (cmd.UserCallback) {
                cmd.UserCallback(cmds, &cmd);
                continue;
            }
            if (cmd.ElemCount == 0) continue;

            if (clipOk) {
                // Clip rect -> window-local -> global boxels -> framebuffer pixels.
                float ax, ay, bx, by;
                MapPoint(xf.mv, xf.pr, xf.vp, (float)mLeft + cmd.ClipRect.x,
                         (float)mTop - cmd.ClipRect.w, &ax, &ay);
                MapPoint(xf.mv, xf.pr, xf.vp, (float)mLeft + cmd.ClipRect.z,
                         (float)mTop - cmd.ClipRect.y, &bx, &by);

                // Normalised, then held inside the window's own pixel rect: the
                // root clip rect is the full client area, so a rounding error
                // must not let a command paint over X-Plane's window frame.
                float x0 = ax < bx ? ax : bx, x1 = ax < bx ? bx : ax;
                float y0 = ay < by ? ay : by, y1 = ay < by ? by : ay;
                if (x0 < winPx[0]) x0 = winPx[0];
                if (y0 < winPx[1]) y0 = winPx[1];
                if (x1 > winPx[2]) x1 = winPx[2];
                if (y1 > winPx[3]) y1 = winPx[3];

                const GLsizei sw = (GLsizei)(x1 - x0), sh = (GLsizei)(y1 - y0);
                // Genuinely fully clipped — the transform is known good here, so
                // this really does mean "nothing of this command is visible".
                if (sw <= 0 || sh <= 0) continue;
                glScissor((GLint)x0, (GLint)y0, sw, sh);
            }

            XPLMBindTexture2d((int)(intptr_t)cmd.GetTexID(), 0);

            glBegin(GL_TRIANGLES);
            for (unsigned e = 0; e < cmd.ElemCount; ++e) {
                const ImDrawVert& v = vtx[idx[cmd.IdxOffset + e] + cmd.VtxOffset];
                const unsigned char* col = (const unsigned char*)&v.col;
                glColor4ub(col[0], col[1], col[2], col[3]);   // ImU32 is RGBA here
                glTexCoord2f(v.uv.x, v.uv.y);
                glVertex2f(v.pos.x, v.pos.y);
            }
            glEnd();
        }
    }

    if (hadCull) glEnable(GL_CULL_FACE);
    glPopMatrix();

    glScissor(oldScissor[0], oldScissor[1], oldScissor[2], oldScissor[3]);
    if (hadScissor) glEnable(GL_SCISSOR_TEST);
    else            glDisable(GL_SCISSOR_TEST);
    glBlendFunc((GLenum)srcRGB, (GLenum)dstRGB);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

void ImguiWindow::FeedMousePos(int globalX, int globalY) {
    ImGui::GetIO().AddMousePosEvent((float)(globalX - mLeft), (float)(mTop - globalY));
}

int ImguiWindow::MouseCb(XPLMWindowID, int x, int y, int status, void* ref) {
    ImguiWindow* self = static_cast<ImguiWindow*>(ref);
    if (!self->Ok()) return 0;
    ScopedContext scoped(self->mCtx);

    self->FeedMousePos(x, y);
    if (status == xplm_MouseDown) {
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        self->mHadMouseDown = true;
        // Text fields need keystrokes, and X-Plane routes those only to the
        // window holding focus. Taken on click rather than on hover so typing
        // into the sim does not get stolen by a window merely passed over.
        XPLMTakeKeyboardFocus(self->mWin);
    } else if (status == xplm_MouseUp) {
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        self->mHadMouseDown = false;
    }
    // Consume: X-Plane must not also treat a drag inside the UI as a click on
    // whatever is behind the window.
    return 1;
}

int ImguiWindow::RightClickCb(XPLMWindowID, int x, int y, int status, void* ref) {
    ImguiWindow* self = static_cast<ImguiWindow*>(ref);
    if (!self->Ok()) return 0;
    ScopedContext scoped(self->mCtx);

    self->FeedMousePos(x, y);
    if (status == xplm_MouseDown)    ImGui::GetIO().AddMouseButtonEvent(1, true);
    else if (status == xplm_MouseUp) ImGui::GetIO().AddMouseButtonEvent(1, false);
    return 1;
}

int ImguiWindow::WheelCb(XPLMWindowID, int x, int y, int wheel, int clicks, void* ref) {
    ImguiWindow* self = static_cast<ImguiWindow*>(ref);
    if (!self->Ok()) return 0;
    ScopedContext scoped(self->mCtx);

    self->FeedMousePos(x, y);
    // XPLM: axis 0 is vertical, 1 is horizontal.
    if (wheel == 0) ImGui::GetIO().AddMouseWheelEvent(0.0f, (float)clicks);
    else            ImGui::GetIO().AddMouseWheelEvent((float)clicks, 0.0f);
    return 1;
}

void ImguiWindow::KeyCb(XPLMWindowID, char key, XPLMKeyFlags flags, char vkey,
                        void* ref, int losingFocus) {
    ImguiWindow* self = static_cast<ImguiWindow*>(ref);
    if (!self->Ok()) return;
    ScopedContext scoped(self->mCtx);
    ImGuiIO& io = ImGui::GetIO();

    if (losingFocus) {
        // Otherwise a modifier held while focus moves away stays latched, and
        // every later click reads as ctrl- or shift-clicked.
        io.AddKeyEvent(ImGuiMod_Shift, false);
        io.AddKeyEvent(ImGuiMod_Ctrl,  false);
        io.AddKeyEvent(ImGuiMod_Alt,   false);
        return;
    }

    const bool down = (flags & xplm_DownFlag) != 0;
    io.AddKeyEvent(ImGuiMod_Shift, (flags & xplm_ShiftFlag) != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl,  (flags & xplm_ControlFlag) != 0);
    io.AddKeyEvent(ImGuiMod_Alt,   (flags & xplm_OptionAltFlag) != 0);

    const ImGuiKey k = VirtualKeyToImGui((unsigned char)vkey);
    if (k != ImGuiKey_None) io.AddKeyEvent(k, down);

    // Printable characters go in as text as well as keys — the key event drives
    // shortcuts and navigation, this drives typing.
    if (down && (unsigned char)key >= 32 && (unsigned char)key < 127)
        io.AddInputCharacter((unsigned int)(unsigned char)key);
}

XPLMCursorStatus ImguiWindow::CursorCb(XPLMWindowID, int x, int y, void* ref) {
    ImguiWindow* self = static_cast<ImguiWindow*>(ref);
    if (!self->Ok()) return xplm_CursorDefault;
    ScopedContext scoped(self->mCtx);

    // Hover has to be fed from here as well as from Draw: without it, ImGui
    // only learns the pointer moved on the next frame, and buttons highlight a
    // frame late.
    self->FeedMousePos(x, y);
    return xplm_CursorArrow;
}
