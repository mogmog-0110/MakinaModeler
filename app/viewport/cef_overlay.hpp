// The HTML shell, drawn over the frame the modeller just rendered.
//
// PLAN.md D-13: the page is displayed by MitiruEngine's CEF layer, used as headers. Makina links
// no engine library -- that layer was decoupled to raw D3D12 handles for exactly this, and the
// claim "the engine's UI layer is reusable" is only worth something once something outside the
// engine has reused it.
//
// This file is the whole of the coupling. Everything the viewport knows about CEF is behind
// `Shell`, and with MITIRU_HAS_CEF undefined the class still exists and does nothing, so the
// call sites in main.cpp carry no `#ifdef`. A modeller without its panels is a worse program,
// not a broken one, and fetching 800 MB of Chromium is not a reasonable prerequisite for
// touching the renderer.
//
// What crosses the boundary, and in which direction:
//
//   C++ -> HTML   the keys ViewState.hpp publishes, pushed once per frame
//   HTML -> C++   the names in data-m-action, handed to a callback the viewport supplies
//
// Nothing else. The tree lives in C++ and the page is a picture of it -- Phase 3's completion
// condition, and the reason the shell has no JavaScript of its own.

#pragma once

#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(MITIRU_HAS_CEF)
#include <mitiru/cef/MitiruCefContext.hpp>
#include <mitiru/input/InputState.hpp>
#endif

namespace app {

/// What the shell sends back: an action name and whatever rode with it.
///
/// The payload is the raw JSON string rather than a parsed object, because the viewport is the
/// only reader and it knows which actions carry what. Parsing here would mean this file learning
/// the vocabulary of every panel.
using ShellAction = std::function<void(const std::string& name, const std::string& payload)>;

/// The page over the frame.
///
/// Not copyable and not movable: it owns a browser process. Nothing in the viewport wants a
/// second one, and a moved-from CEF context is a shutdown that never happens.
class Shell {
public:
    Shell() = default;
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    ~Shell() { shutdown(); }

    /// Starts the browser and loads `exeDir/ui/shell.html`.
    ///
    /// Returns false when there is no CEF in this build, or when it failed to start. Both are
    /// the same thing to the caller -- draw the scene, skip the panels -- so the difference is
    /// only in what gets printed.
    bool start(ID3D12Device* device, ID3D12CommandQueue* queue, const std::string& exeDir,
               int width, int height, ShellAction onAction) {
#if defined(MITIRU_HAS_CEF)
        m_onAction = std::move(onAction);
        // app://, not file://. The engine's scheme handler serves <exe>/assets and the
        // bridge is wired for that origin; a file:// page renders and its cefQuery goes
        // nowhere, which looks exactly like a button that does nothing.
        const std::string url = "app://ui/shell.html";
        if (!m_ctx.initialize(device, queue, exeDir, exeDir + "/makina_cef.log", width, height,
                              url)) {
            std::fprintf(stderr, "warning: the shell did not start; drawing without it\n");
            return false;
        }
        // Through a store rather than executeJavaScript directly, because the page loads
        // asynchronously: everything published before the document is ready would otherwise be
        // dropped, and the first thing published is the whole outliner. The store re-sends its
        // snapshot on load-end, so the panels come up filled rather than empty until the first
        // edit.
        m_store = m_ctx.makeStateStore();
        m_running = true;
        return true;
#else
        (void)device; (void)queue; (void)exeDir; (void)width; (void)height; (void)onAction;
        std::printf("built without CEF: the viewport draws, the panels are absent\n");
        return false;
#endif
    }

    /// Registers one action name the page may dispatch.
    ///
    /// Named one at a time rather than catching everything, so a page that asks for something
    /// the viewport does not implement is silent here instead of reaching a handler that shrugs.
    /// shell_audit.py already refuses a page naming an action the keymap does not know; this is
    /// the same rule enforced where the message actually arrives.
    ///
    /// On the store, not on the context. `data-m-action` goes through `mitiru.dispatch`, which
    /// sends one cefQuery called `state.dispatch` carrying `{action, payload}` -- the action name
    /// is inside the message, not the name of the message. Registering a cefQuery handler called
    /// `edit.toggleMute` therefore waits for something the page never sends, and the button looks
    /// dead while everything else about it works.
    void accept(const std::string& name) {
#if defined(MITIRU_HAS_CEF)
        if (!m_running || !m_store) {
            return;
        }
        m_store->onAction(name, [this, name](const mitiru::cef::json& payload) {
            if (m_onAction) {
                m_onAction(name, payload.dump());
            }
            return mitiru::cef::json::object();
        });
#else
        (void)name;
#endif
    }

    /// Pumps CEF and uploads whatever the page repainted.
    ///
    /// Called once per frame before recording. Uploading after the command list has been closed
    /// would put this frame's picture on the next frame's screen.
    void pump() {
#if defined(MITIRU_HAS_CEF)
        if (!m_running) {
            return;
        }
        m_ctx.doMessageLoopWork();
        if (m_ctx.hasDirtyFrame()) {
            m_ctx.upload();
        }
#endif
    }

    /// Pushes state to the page, sending only what changed.
    ///
    /// The outliner's JSON is several kilobytes for a real model and most frames change none of
    /// it. Sending it anyway would put a string compare and a bridge crossing on every frame for
    /// a tree that moves when the user edits, which is rarely.
    void publish(const std::vector<std::pair<std::string, std::string>>& entries) {
#if defined(MITIRU_HAS_CEF)
        if (!m_running || !m_store) {
            return;
        }
        for (const auto& kv : entries) {
            auto it = m_last.find(kv.first);
            if (it != m_last.end() && it->second == kv.second) {
                continue;
            }
            m_last[kv.first] = kv.second;
            // As a string. The binder parses a value that starts with '[' or '{' (parseValue in
            // mitiru_bind.js), which is how the outliner's array arrives as an array.
            m_store->set(kv.first, kv.second);
        }
#else
        (void)entries;
#endif
    }

    /// Hands the pointer to the page, and says whether the page took it.
    ///
    /// Returns true when the cursor is over something the page drew, which is the viewport's cue
    /// to leave the mouse alone: dragging on the outliner must not also orbit the camera.
    ///
    /// The test is the page's own alpha at the cursor, not a rectangle kept here. The shell's
    /// layout is a CSS grid and the day a panel moves, a copy of its bounds in this file would
    /// still be the old ones -- and the failure is a strip of window that looks like a panel and
    /// orbits when clicked, which nothing would catch.
    ///
    /// Buttons only. Wheel is deliberately left to the viewport: the page has nothing that
    /// scrolls yet except the outliner, and taking the wheel away from dolly is the kind of
    /// change a modeller notices immediately.
    [[nodiscard]] bool takePointer(int x, int y, bool left, bool middle, bool right) {
#if defined(MITIRU_HAS_CEF)
        if (!m_running) {
            return false;
        }
        m_input.beginFrame();
        m_input.setMousePosition(static_cast<float>(x), static_cast<float>(y));
        m_input.setMouseButtonDown(mitiru::MouseButton::Left, left);
        m_input.setMouseButtonDown(mitiru::MouseButton::Middle, middle);
        m_input.setMouseButtonDown(mitiru::MouseButton::Right, right);
        m_ctx.handleInput(m_input);
        return m_ctx.pointerOverUi(x, y);
#else
        (void)x; (void)y; (void)left; (void)middle; (void)right;
        return false;
#endif
    }

    /// Records the page over the frame, into the caller's command list.
    void record(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, int width,
                int height) {
#if defined(MITIRU_HAS_CEF)
        if (m_running) {
            m_ctx.recordComposite(cl, rtv, width, height);
        }
#else
        (void)cl; (void)rtv; (void)width; (void)height;
#endif
    }

    void resize(int width, int height) {
#if defined(MITIRU_HAS_CEF)
        if (m_running) {
            m_ctx.resize(width, height);
        }
#else
        (void)width; (void)height;
#endif
    }

    void shutdown() {
#if defined(MITIRU_HAS_CEF)
        if (m_running) {
            m_ctx.shutdown();
            m_running = false;
        }
#endif
    }

    [[nodiscard]] bool running() const noexcept { return m_running; }

    /// Whether the page has drawn anything yet.
    ///
    /// The CEF layer refuses input until its first paint, so a scripted click before this is
    /// silently dropped -- and a check built on a frame number would call a working button
    /// broken on any machine where Chromium starts a little slower.
    ///
    /// The paint count, not the load state. Load-end fires when the document is ready, which is
    /// several frames before anything has been rasterised; asking that question instead made the
    /// first scripted click land while the hit test still saw a blank page, and it fell through
    /// to the viewport and cleared the selection.
    [[nodiscard]] bool painted() const noexcept {
#if defined(MITIRU_HAS_CEF)
        return m_running && m_ctx.paintStats().paintCount > 0;
#else
        return false;
#endif
    }

private:
    bool        m_running = false;
    ShellAction m_onAction;
#if defined(MITIRU_HAS_CEF)
    mitiru::cef::MitiruCefContext                m_ctx;
    /// Rebuilt each frame from the viewport's own input, because CEF wants the engine's shape
    /// of it and the viewport has its own. Kept as a member rather than a local so the
    /// press-and-release edges beginFrame() derives survive between frames.
    mitiru::InputState                           m_input;
    std::shared_ptr<mitiru::cef::StateStore>     m_store;
    /// The last value pushed per key, so an unchanged one is not pushed again.
    std::map<std::string, std::string>           m_last;
#endif
};

}  // namespace app
