#include "TwoPanePersistent.hpp"

#define private public
#include <hyprland/src/Compositor.hpp>
#undef private

// ── Focus hook ────────────────────────────────────────────────────────────────

static CFunctionHook* g_pFocusHook = nullptr;
typedef void (*tFocusWindow)(void*, PHLWINDOW, PHLWINDOW);

void hkFocusWindow(void* self, PHLWINDOW pWindow, PHLWINDOW pSurface) {
    (*(tFocusWindow)g_pFocusHook->m_original)(self, pWindow, pSurface);

    if (!g_pTPPAlgo || !pWindow) return;

    auto pWs = pWindow->m_workspace;
    if (!pWs) return;

    auto space = pWs->m_space;
    if (!space) return;

    for (auto& wt : space->targets()) {
        auto t = wt.lock();
        if (t && t->window() == pWindow) {
            g_pTPPAlgo->onTargetFocused(t);
            return;
        }
    }
}

// ── Plugin entry ──────────────────────────────────────────────────────────────

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // GIT_COMMIT_HASH is defined by hyprland/src/version.h (included via PluginAPI.hpp)
    const std::string HASH = __hyprland_api_get_hash();
    if (HASH != GIT_COMMIT_HASH) {
        HyprlandAPI::addNotification(PHANDLE,
            "[TwoPanePersistent] ABI mismatch — got: " + HASH + " expected: " + GIT_COMMIT_HASH,
            CHyprColor{1.f, 0.8f, 0.0f, 1.f}, 5000);
        // Don't abort — hyprpm already verified compatibility
    }

    // Register using the correct 0.54 API signature:
    // addTiledAlgo(handle, name, typeid, factory)
    HyprlandAPI::addTiledAlgo(PHANDLE, "TwoPanePersistent", &typeid(CTPPAlgorithm),
        []() -> UP<Layout::ITiledAlgorithm> {
            auto algo = makeUnique<CTPPAlgorithm>();
            g_pTPPAlgo = algo.get();
            return algo;
        });

    // Hook focusWindow for slave persistence tracking
    auto methods = HyprlandAPI::findFunctionsByName(PHANDLE, "focusWindow");
    if (!methods.empty()) {
        g_pFocusHook = HyprlandAPI::createFunctionHook(
            PHANDLE, methods[0].address, (void*)hkFocusWindow);
        g_pFocusHook->hook();
    } else {
        HyprlandAPI::addNotification(PHANDLE,
            "[TwoPanePersistent] focusWindow hook failed — persistence disabled",
            CHyprColor{1.f, 0.6f, 0.f, 1.f}, 5000);
    }

    HyprlandAPI::addNotification(PHANDLE,
        "[TwoPanePersistent] Loaded! Use `general { layout = TwoPanePersistent }`",
        CHyprColor{0.2f, 0.9f, 0.2f, 1.f}, 5000);

    return {"TwoPanePersistent",
            "XMonad-style TwoPanePersistent layout",
            "tomjpalamattam", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_pFocusHook)
        g_pFocusHook->unhook();
    g_pTPPAlgo = nullptr;
}