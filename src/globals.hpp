#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>

inline HANDLE PHANDLE = nullptr;

struct STPPGlobalState {
    struct {
        SP<Config::Values::CFloatValue> mfact;      // master pane fraction of the work area
        SP<Config::Values::CFloatValue> dfact;      // step used by `mfact +` / `mfact -`
    } config;
};

inline UP<STPPGlobalState> g_pTPPState;
