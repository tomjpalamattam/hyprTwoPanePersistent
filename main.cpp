#include "TwoPanePersistent.hpp"

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/LayoutManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#undef private

// ── Focus hook ────────────────────────────────────────────────────────────────
// We hook focusWindow so we can intercept every focus change and update the
// pinned slave without polling.

inline CFunctionHook* g_pFocusWindowHook = nullptr;

typedef void (*origFocusWindow)(void*, PHLWINDOW, PHLWINDOW);

void hkFocusWindow(void* thisptr, PHLWINDOW pWindow, PHLWINDOW pSurface) {
    // Call original first so Hyprland's internal state is updated
    (*(origFocusWindow)g_pFocusWindowHook->m_pOriginal)(thisptr, pWindow, pSurface);

    // Now inform our layout
    if (g_pTPPLayout && pWindow)
        g_pTPPLayout->onWindowFocused(pWindow);
}

// ── Dispatchers ───────────────────────────────────────────────────────────────

static SDispatchResult dispatchCycleNext(std::string args) {
    auto pWindow = g_pCompositor->m_pLastWindow.lock();
    if (pWindow && g_pTPPLayout)
        g_pTPPLayout->cycleNext(pWindow->m_pWorkspace);
    return {};
}

static SDispatchResult dispatchCyclePrev(std::string args) {
    auto pWindow = g_pCompositor->m_pLastWindow.lock();
    if (pWindow && g_pTPPLayout)
        g_pTPPLayout->cyclePrev(pWindow->m_pWorkspace);
    return {};
}

// ── Plugin init / deinit ──────────────────────────────────────────────────────

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // Verify ABI compatibility
    const std::string HASH = __hyprland_api_get_hash();
    if (HASH != GIT_COMMIT_HASH) {
        HyprlandAPI::addNotification(PHANDLE,
            "[TwoPanePersistent] ABI mismatch! Rebuild against your Hyprland version.",
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 10000);
        throw std::runtime_error("ABI mismatch");
    }

    // Create layout instance and register it
    g_pTPPLayout = new CTwoPanePersistentLayout();
    HyprlandAPI::addLayout(PHANDLE, "TwoPanePersistent", g_pTPPLayout);

    // Hook focusWindow to intercept focus changes
    static const auto METHODS = HyprlandAPI::findFunctionsByName(PHANDLE, "focusWindow");
    if (METHODS.empty()) {
        HyprlandAPI::addNotification(PHANDLE,
            "[TwoPanePersistent] Could not find focusWindow — focus tracking disabled.",
            CHyprColor{1.0, 0.6, 0.0, 1.0}, 5000);
    } else {
        g_pFocusWindowHook = HyprlandAPI::createFunctionHook(
            handle, METHODS[0].address, (void*)&hkFocusWindow);
        g_pFocusWindowHook->hook();
    }

    // Register dispatchers for cycle keybinds:
    //   bind = $mod, Tab,   exec, hyprctl dispatch tpp-cyclenext
    //   bind = $mod SHIFT, Tab, exec, hyprctl dispatch tpp-cycleprev
    HyprlandAPI::addDispatcher(PHANDLE, "tpp-cyclenext", dispatchCycleNext);
    HyprlandAPI::addDispatcher(PHANDLE, "tpp-cycleprev", dispatchCyclePrev);

    HyprlandAPI::addNotification(PHANDLE,
        "[TwoPanePersistent] Loaded! Set `general:layout = TwoPanePersistent` to use.",
        CHyprColor{0.2, 0.9, 0.2, 1.0}, 5000);

    return {"TwoPanePersistent",
            "XMonad-style TwoPanePersistent layout for Hyprland",
            "you", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_pFocusWindowHook)
        g_pFocusWindowHook->unhook();

    // Layout and dispatcher cleanup is handled by Hyprland on plugin unload
    delete g_pTPPLayout;
    g_pTPPLayout = nullptr;
}
