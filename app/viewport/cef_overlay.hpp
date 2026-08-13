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
        const std::string url = "file:///" + exeDir + "/ui/shell.html";
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
    void accept(const std::string& name) {
#if defined(MITIRU_HAS_CEF)
        if (!m_running) {
            return;
        }
        m_ctx.registerHandler(name, [this, name](std::string_view payload) {
            if (m_onAction) {
                m_onAction(name, std::string(payload));
            }
            return std::string("{}");
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

private:
    bool        m_running = false;
    ShellAction m_onAction;
#if defined(MITIRU_HAS_CEF)
    mitiru::cef::MitiruCefContext                m_ctx;
    std::shared_ptr<mitiru::cef::StateStore>     m_store;
    /// The last value pushed per key, so an unchanged one is not pushed again.
    std::map<std::string, std::string>           m_last;
#endif
};

}  // namespace app
