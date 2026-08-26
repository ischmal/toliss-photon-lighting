#include "imgui_win32_gl.h"

#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <GL/gl.h>

#include <cstddef>
#include <string>

namespace photon {
namespace studio {
namespace {

const char kClassName[] = "PhotonLitStudioWindow";

Window* gActive = nullptr;

// VK_* to ImGuiKey. Only what a tuning bench needs: text entry into the numeric
// fields, and the navigation keys ImGui itself wires up.
ImGuiKey KeyFromVk(WPARAM vk) {
    switch (vk) {
        case VK_TAB: return ImGuiKey_Tab;
        case VK_LEFT: return ImGuiKey_LeftArrow;
        case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow;
        case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End;
        case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete;
        case VK_BACK: return ImGuiKey_Backspace;
        case VK_SPACE: return ImGuiKey_Space;
        case VK_RETURN: return ImGuiKey_Enter;
        case VK_ESCAPE: return ImGuiKey_Escape;
        case VK_CONTROL: return ImGuiKey_LeftCtrl;
        case VK_SHIFT: return ImGuiKey_LeftShift;
        case VK_MENU: return ImGuiKey_LeftAlt;
        case VK_OEM_MINUS: return ImGuiKey_Minus;
        case VK_OEM_PERIOD: return ImGuiKey_Period;
        case VK_OEM_COMMA: return ImGuiKey_Comma;
        default: break;
    }
    if (vk >= '0' && vk <= '9')
        return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - VK_NUMPAD0));
    return ImGuiKey_None;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Messages can arrive before ImGui has a context (WM_CREATE, WM_SIZE during
    // CreateWindowEx). Touching io then would dereference null.
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    ImGuiIO* io = ctx ? &ImGui::GetIO() : nullptr;

    switch (msg) {
        case WM_CLOSE:
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            return 0;
        case WM_MOUSEMOVE:
            if (io)
                io->AddMousePosEvent(static_cast<float>(GET_X_LPARAM(lp)),
                                     static_cast<float>(GET_Y_LPARAM(lp)));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            if (io) {
                const int b = (msg == WM_LBUTTONDOWN)   ? 0
                              : (msg == WM_RBUTTONDOWN) ? 1
                                                        : 2;
                io->AddMouseButtonEvent(b, true);
            }
            // Capture so a drag that leaves the client area keeps reporting —
            // without it a slider "sticks" the moment the pointer exits.
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP: {
            if (io) {
                const int b = (msg == WM_LBUTTONUP)   ? 0
                              : (msg == WM_RBUTTONUP) ? 1
                                                      : 2;
                io->AddMouseButtonEvent(b, false);
            }
            ReleaseCapture();
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (io)
                io->AddMouseWheelEvent(
                    0.0f, static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) /
                              static_cast<float>(WHEEL_DELTA));
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            if (io) {
                const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                const ImGuiKey key = KeyFromVk(wp);
                if (key != ImGuiKey_None) io->AddKeyEvent(key, down);
                // The modifier aliases ImGui tests directly.
                io->AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
                io->AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                io->AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
            }
            return 0;
        }
        case WM_CHAR:
            if (io && wp > 0 && wp < 0x10000)
                io->AddInputCharacterUTF16(static_cast<unsigned short>(wp));
            return 0;
        case WM_KILLFOCUS:
            // Otherwise a key held while alt-tabbing away stays down forever.
            if (io) io->ClearInputKeys();
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

double NowSeconds() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(freq.QuadPart);
}

}  // namespace

Window::~Window() { Destroy(); }

bool Window::Create(const char* title, int width, int height,
                    std::string& error) {
    HINSTANCE inst = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;  // one DC for the window's life; the GL context holds it
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExA(&wc);  // a duplicate registration is harmless here

    RECT r = {0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExA(0, kClassName, title, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, inst,
                                nullptr);
    if (!hwnd) {
        error = "CreateWindowEx failed";
        return false;
    }
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 0;
    const int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) {
        error = "no usable OpenGL pixel format";
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return false;
    }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) {
        error = "cannot create an OpenGL context";
        if (rc) wglDeleteContext(rc);
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return false;
    }

    hwnd_ = hwnd;
    dc_ = dc;
    rc_ = rc;
    width_ = width;
    height_ = height;
    gActive = this;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "photon-win32";
    io.BackendRendererName = "photon-gl1";
    io.IniFilename = nullptr;  // a bench tool should open the same way every time
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // ⚠ ImGuiBackendFlags_RendererHasTextures is deliberately NOT set; see the
    // header. The atlas is uploaded once, here, the legacy way.
    unsigned char* px = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);
    glGenTextures(1, &fontTex_);
    glBindTexture(GL_TEXTURE_2D, fontTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);
    io.Fonts->SetTexID(static_cast<ImTextureID>(
        static_cast<std::intptr_t>(fontTex_)));

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    lastTime_ = NowSeconds();
    return true;
}

bool Window::Pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) closed_ = true;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return !closed_;
}

void Window::BeginFrame() {
    RECT rc;
    GetClientRect(static_cast<HWND>(hwnd_), &rc);
    width_ = rc.right - rc.left;
    height_ = rc.bottom - rc.top;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width_ > 0 ? width_ : 1),
                            static_cast<float>(height_ > 0 ? height_ : 1));
    const double now = NowSeconds();
    // A clamped, non-zero delta: ImGui divides by it, and a debugger pause or a
    // long recolor on the UI thread would otherwise hand it a huge or zero step.
    double dt = now - lastTime_;
    if (dt <= 0.0) dt = 1.0 / 60.0;
    if (dt > 0.25) dt = 0.25;
    io.DeltaTime = static_cast<float>(dt);
    lastTime_ = now;

    ImGui::NewFrame();
}

void Window::EndFrame(const float clear[3]) {
    ImGui::Render();
    ImDrawData* data = ImGui::GetDrawData();

    glViewport(0, 0, width_, height_);
    glClearColor(clear[0], clear[1], clear[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (data && data->CmdLists.Size > 0) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LIGHTING);
        glEnable(GL_SCISSOR_TEST);
        glEnable(GL_TEXTURE_2D);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(width_), static_cast<double>(height_),
                0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        // ⚠ CmdLists, not CmdListsCount. See the header.
        for (int n = 0; n < data->CmdLists.Size; ++n) {
            const ImDrawList* list = data->CmdLists[n];
            const ImDrawVert* vtx = list->VtxBuffer.Data;
            const ImDrawIdx* idx = list->IdxBuffer.Data;
            glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                            reinterpret_cast<const char*>(vtx) +
                                offsetof(ImDrawVert, pos));
            glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                              reinterpret_cast<const char*>(vtx) +
                                  offsetof(ImDrawVert, uv));
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert),
                           reinterpret_cast<const char*>(vtx) +
                               offsetof(ImDrawVert, col));
            for (int c = 0; c < list->CmdBuffer.Size; ++c) {
                const ImDrawCmd& cmd = list->CmdBuffer[c];
                if (cmd.UserCallback) {
                    cmd.UserCallback(list, &cmd);
                    continue;
                }
                const int x0 = static_cast<int>(cmd.ClipRect.x);
                const int y0 = static_cast<int>(cmd.ClipRect.y);
                const int x1 = static_cast<int>(cmd.ClipRect.z);
                const int y1 = static_cast<int>(cmd.ClipRect.w);
                if (x1 <= x0 || y1 <= y0) continue;
                // GL's scissor origin is the BOTTOM left; ImGui's clip rects are
                // top-left. Getting this wrong clips the UI to a mirrored band.
                glScissor(x0, height_ - y1, x1 - x0, y1 - y0);
                glBindTexture(GL_TEXTURE_2D,
                              static_cast<GLuint>(
                                  static_cast<std::intptr_t>(cmd.GetTexID())));
                glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.ElemCount),
                               sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT
                                                      : GL_UNSIGNED_INT,
                               idx + cmd.IdxOffset);
            }
        }

        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisable(GL_SCISSOR_TEST);
    }

    SwapBuffers(static_cast<HDC>(dc_));
}

void Window::Destroy() {
    if (!hwnd_) return;
    if (ImGui::GetCurrentContext()) {
        if (fontTex_) {
            glDeleteTextures(1, &fontTex_);
            fontTex_ = 0;
        }
        ImGui::DestroyContext();
    }
    wglMakeCurrent(nullptr, nullptr);
    if (rc_) wglDeleteContext(static_cast<HGLRC>(rc_));
    if (dc_) ReleaseDC(static_cast<HWND>(hwnd_), static_cast<HDC>(dc_));
    DestroyWindow(static_cast<HWND>(hwnd_));
    hwnd_ = dc_ = rc_ = nullptr;
    if (gActive == this) gActive = nullptr;
}

TextureId Window::CreateTexture(int w, int h, const std::uint8_t* rgba) {
    GLuint id = 0;
    glGenTextures(1, &id);
    if (!id) return 0;
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba);
    return id;
}

void Window::UpdateTexture(TextureId id, int w, int h,
                           const std::uint8_t* rgba) {
    if (!id) return;
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                    rgba);
}

void Window::DestroyTexture(TextureId id) {
    if (!id) return;
    GLuint g = id;
    glDeleteTextures(1, &g);
}

int Window::MaxTextureSize() {
    GLint v = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
    return v > 0 ? v : 1024;
}

}  // namespace studio
}  // namespace photon
