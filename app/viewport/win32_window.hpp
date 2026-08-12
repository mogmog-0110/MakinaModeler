// A window and the input that arrives through it.
//
// Deliberately small and deliberately ours. The plan keeps Makina and MitiruEngine independent
// (PLAN.md 3.1), and the engine's window comes attached to a renderer built for games -- a frame
// graph, mesh draws, a game DLL boundary. The modeller needs one custom pass with a generated
// shader, so borrowing that machinery would mean fighting it.
//
// What this does is turn Win32 messages into a state a frame can read: which buttons are down,
// how far the cursor moved since the last frame, which keys went down this frame. The distinction
// between "is down" and "went down this frame" is the whole reason it exists -- an orbit needs the
// first, a keyboard shortcut needs the second, and reading the wrong one gives either a shortcut
// that fires sixty times a second or an orbit that moves one frame in sixty.

#pragma once

// NOMINMAX and WIN32_LEAN_AND_MEAN come from the build, because d3d12.h includes windows.h as
// well and whichever header lands first would otherwise have to carry them. Repeated here only so
// this file still compiles if it is included on its own.
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

// The wide form of IDC_ARROW. windows.h only spells it this way when UNICODE is defined, and this
// build leaves the ANSI/wide choice to each call rather than defining it globally.
#define IDC_ARROW_W MAKEINTRESOURCEW(32512)

#include <string>
#include <unordered_map>
#include <vector>

namespace app {

/// What happened since the last frame.
struct FrameInput {
    /// Cursor movement in screen fractions: a drag across the full width is 1.0. Fractions rather
    /// than pixels so the same gesture covers the same arc whatever the window size.
    double dx = 0.0;
    double dy = 0.0;
    /// Cursor position, also as fractions, measured from the centre with +y up. This is the
    /// convention Camera.hpp and Pick.hpp take, so nothing converts between two of them.
    double cursorU = 0.0;
    double cursorV = 0.0;
    double wheel = 0.0;

    bool leftDown = false;
    bool middleDown = false;
    bool rightDown = false;

    bool leftPressed = false;   ///< went down this frame
    bool middlePressed = false;
    bool rightPressed = false;

    bool shift = false;
    bool ctrl = false;
    bool alt = false;

    /// Keys that went down this frame, as the uppercase names Keymap.hpp uses.
    std::vector<std::string> keysPressed;

    int width = 0;
    int height = 0;

    [[nodiscard]] double aspect() const {
        return height > 0 ? static_cast<double>(width) / static_cast<double>(height) : 1.0;
    }
};

class Window {
public:
    Window(const wchar_t* title, int width, int height) : m_width(width), m_height(height) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &Window::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        // IDC_ARROW is a MAKEINTRESOURCE, which is char* unless UNICODE is defined. The build
// does not define it, so the wide entry point needs the wide spelling explicitly.
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW_W);
        wc.lpszClassName = L"MakinaViewport";
        RegisterClassExW(&wc);

        RECT rc{0, 0, width, height};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        m_hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                                 nullptr, wc.hInstance, this);
        ShowWindow(m_hwnd, SW_SHOW);
    }

    ~Window() {
        if (m_hwnd != nullptr) {
            DestroyWindow(m_hwnd);
        }
    }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] HWND handle() const noexcept { return m_hwnd; }
    [[nodiscard]] bool alive() const noexcept { return !m_closed; }
    [[nodiscard]] int width() const noexcept { return m_width; }
    [[nodiscard]] int height() const noexcept { return m_height; }
    [[nodiscard]] bool consumeResize() {
        const bool r = m_resized;
        m_resized = false;
        return r;
    }

    /// Drains the message queue and returns what accumulated.
    ///
    /// The per-frame parts are cleared here rather than by the caller: forgetting to clear a
    /// "pressed" flag turns a single key press into a key that is held forever, and that is a
    /// mistake worth making impossible rather than documenting.
    FrameInput pump() {
        m_input.dx = 0.0;
        m_input.dy = 0.0;
        m_input.wheel = 0.0;
        m_input.leftPressed = false;
        m_input.middlePressed = false;
        m_input.rightPressed = false;
        m_input.keysPressed.clear();

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_closed = true;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        m_input.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        m_input.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        m_input.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        m_input.width = m_width;
        m_input.height = m_height;
        return m_input;
    }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Window* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<Window*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (self != nullptr) {
            return self->handle(hwnd, msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CLOSE:
                m_closed = true;
                return 0;

            case WM_SIZE: {
                const int w = LOWORD(lp);
                const int h = HIWORD(lp);
                // A minimised window reports zero, and a swap chain of zero width is an error
                // rather than a small window. Ignored until it comes back.
                if (w > 0 && h > 0 && (w != m_width || h != m_height)) {
                    m_width = w;
                    m_height = h;
                    m_resized = true;
                }
                return 0;
            }

            case WM_MOUSEMOVE: {
                const int x = GET_X_LPARAM(lp);
                const int y = GET_Y_LPARAM(lp);
                if (m_haveCursor) {
                    m_input.dx += static_cast<double>(x - m_lastX) / m_width;
                    m_input.dy += static_cast<double>(y - m_lastY) / m_height;
                }
                m_lastX = x;
                m_lastY = y;
                m_haveCursor = true;
                m_input.cursorU = static_cast<double>(x) / m_width - 0.5;
                m_input.cursorV = 0.5 - static_cast<double>(y) / m_height;
                return 0;
            }

            case WM_LBUTTONDOWN:
                m_input.leftDown = true;
                m_input.leftPressed = true;
                // Capture, or a drag that leaves the window stops sending moves and the camera
                // freezes mid-orbit until the button is released somewhere else.
                SetCapture(hwnd);
                return 0;
            case WM_LBUTTONUP:
                m_input.leftDown = false;
                releaseIfIdle();
                return 0;
            case WM_MBUTTONDOWN:
                m_input.middleDown = true;
                m_input.middlePressed = true;
                SetCapture(hwnd);
                return 0;
            case WM_MBUTTONUP:
                m_input.middleDown = false;
                releaseIfIdle();
                return 0;
            case WM_RBUTTONDOWN:
                m_input.rightDown = true;
                m_input.rightPressed = true;
                SetCapture(hwnd);
                return 0;
            case WM_RBUTTONUP:
                m_input.rightDown = false;
                releaseIfIdle();
                return 0;

            case WM_MOUSEWHEEL:
                m_input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / static_cast<double>(WHEEL_DELTA);
                return 0;

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                // Auto-repeat is filtered out. A held key that fires every message would make
                // "delete" delete a whole chain and "undo" walk the history to its start.
                if ((lp & (1 << 30)) == 0) {
                    const std::string name = keyName(static_cast<int>(wp));
                    if (!name.empty()) {
                        m_input.keysPressed.push_back(name);
                    }
                }
                // Alt would otherwise open the system menu and swallow the key.
                return msg == WM_SYSKEYDOWN ? 0 : DefWindowProcW(hwnd, msg, wp, lp);
            }

            default:
                break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void releaseIfIdle() {
        if (!m_input.leftDown && !m_input.middleDown && !m_input.rightDown) {
            ReleaseCapture();
        }
    }

    /// Virtual key to the uppercase names a keymap file uses.
    static std::string keyName(int vk) {
        if (vk >= 'A' && vk <= 'Z') {
            return std::string(1, static_cast<char>(vk));
        }
        if (vk >= '0' && vk <= '9') {
            return std::string(1, static_cast<char>(vk));
        }
        switch (vk) {
            case VK_NUMPAD0: return "NUMPAD0";
            case VK_NUMPAD1: return "NUMPAD1";
            case VK_NUMPAD2: return "NUMPAD2";
            case VK_NUMPAD3: return "NUMPAD3";
            case VK_NUMPAD4: return "NUMPAD4";
            case VK_NUMPAD5: return "NUMPAD5";
            case VK_NUMPAD6: return "NUMPAD6";
            case VK_NUMPAD7: return "NUMPAD7";
            case VK_NUMPAD8: return "NUMPAD8";
            case VK_NUMPAD9: return "NUMPAD9";
            case VK_DECIMAL: return "NUMPADPERIOD";
            case VK_HOME:    return "HOME";
            case VK_DELETE:  return "DELETE";
            case VK_ESCAPE:  return "ESCAPE";
            case VK_RETURN:  return "ENTER";
            case VK_TAB:     return "TAB";
            default:         return std::string();
        }
    }

    HWND       m_hwnd = nullptr;
    int        m_width = 0;
    int        m_height = 0;
    bool       m_closed = false;
    bool       m_resized = false;
    FrameInput m_input;
    int        m_lastX = 0;
    int        m_lastY = 0;
    bool       m_haveCursor = false;
};

}  // namespace app
