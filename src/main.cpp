#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include "TwoPanePersistent.hpp"

#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>

#include <stdexcept>
#include <typeinfo>

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[twopanepersistent] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[twopanepersistent] Version mismatch");
    }

    g_pTPPState = makeUnique<STPPGlobalState>();

    g_pTPPState->config.mfact = makeShared<Config::Values::CFloatValue>("plugin:twopanepersistent:mfact", "Fraction of the work area taken by the master pane", 0.5F,
                                                                       Config::Values::SFloatValueOptions{.min = 0.05F, .max = 0.95F});
    g_pTPPState->config.dfact = makeShared<Config::Values::CFloatValue>("plugin:twopanepersistent:dfact", "Step size used by `mfact +` and `mfact -`", 0.03F,
                                                                       Config::Values::SFloatValueOptions{.min = 0.001F, .max = 0.5F});
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pTPPState->config.mfact);
    HyprlandAPI::addConfigValueV2(PHANDLE, g_pTPPState->config.dfact);

    const bool OK = HyprlandAPI::addTiledAlgo(PHANDLE, "twopanepersistent", &typeid(Layout::Tiled::CTwoPanePersistent),
                                              []() -> UP<Layout::ITiledAlgorithm> { return makeUnique<Layout::Tiled::CTwoPanePersistent>(); });

    if (!OK) {
        HyprlandAPI::addNotification(PHANDLE, "[twopanepersistent] Failed to register the layout", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[twopanepersistent] addTiledAlgo failed");
    }

    return {"twopanepersistent", "XMonad's TwoPanePersistent layout for Hyprland", "you", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    HyprlandAPI::removeAlgo(PHANDLE, "twopanepersistent");
    g_pTPPState.reset();
}
