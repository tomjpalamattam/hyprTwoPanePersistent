#pragma once

#define private public
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/layout/IHyprLayout.hpp>
#include <hyprland/src/desktop/Window.hpp>
#include <hyprland/src/managers/LayoutManager.hpp>
#undef private

#include <list>
#include <deque>
#include <unordered_map>

// Per-window node tracked by our layout
struct STPPNodeData {
    PHLWINDOWREF pWindow;

    // Is this the master (left pane)?
    bool isMaster = false;

    // Computed geometry (written by recalculate, read by applyNodeDataToWindow)
    Vector2D position;
    Vector2D size;

    int  workspaceID = -1;
    bool valid       = true;
};

// Per-workspace state
struct STPPWorkspaceData {
    int   workspaceID = -1;

    // The pinned slave: whichever non-master window was most recently focused.
    // nullopt = not set yet, pick the first slave.
    PHLWINDOWREF pinnedSlave;

    // Hidden windows (all non-master, non-slave tiled windows), in cycle order.
    // Front = next to swap in on --cycle.
    std::deque<PHLWINDOWREF> hiddenQueue;

    // Split fraction: master takes mfact of width, slave gets the rest.
    float mfact = 0.55f;
};

class CTwoPanePersistentLayout : public IHyprLayout {
  public:
    // ── IHyprLayout interface ─────────────────────────────────────────────
    virtual void    onWindowCreatedTiling(PHLWINDOW, eDirection direction = DIRECTION_DEFAULT) override;
    virtual void    onWindowRemovedTiling(PHLWINDOW) override;
    virtual bool    isWindowTiled(PHLWINDOW) override;
    virtual void    recalculateMonitor(const MONITORID&) override;
    virtual void    recalculateWindow(PHLWINDOW) override;
    virtual void    resizeActiveWindow(const Vector2D&, eRectCorner corner, PHLWINDOW pWindow = nullptr) override;
    virtual void    fullscreenRequestForWindow(PHLWINDOW, const eFullscreenMode, const eFullscreenMode) override;
    virtual std::any layoutMessage(SLayoutMessageHeader, std::string) override;
    virtual SWindowRenderLayoutHints requestRenderHints(PHLWINDOW) override;
    virtual void    switchWindows(PHLWINDOW, PHLWINDOW) override;
    virtual void    moveWindowTo(PHLWINDOW, const std::string& dir, bool silent) override;
    virtual void    alterSplitRatio(PHLWINDOW, float, bool) override;
    virtual std::string getLayoutName() override;
    virtual void    replaceWindowDataWith(PHLWINDOW from, PHLWINDOW to) override;
    virtual Vector2D predictSizeForNewWindowTiled() override;

    // Called by our focus hook when a window gains focus
    void onWindowFocused(PHLWINDOW pWindow);

    // Cycle: swap current slave with the next in the hidden queue
    void cycleNext(PHLWORKSPACE pWorkspace);
    void cyclePrev(PHLWORKSPACE pWorkspace);

  private:
    std::list<STPPNodeData>                          m_lNodes;
    std::unordered_map<int, STPPWorkspaceData>       m_mWorkspaceData;

    STPPNodeData*        getNodeFromWindow(PHLWINDOW);
    STPPWorkspaceData&   getOrCreateWorkspaceData(int wsID);
    STPPNodeData*        getMasterNode(int wsID);
    STPPNodeData*        getSlaveNode(int wsID);   // the pinned slave currently on-screen
    PHLWINDOW            getPinnedSlave(int wsID); // returns null if pin is stale

    void applyNodeDataToWindow(STPPNodeData* node);
    void recalculateWorkspace(PHLWORKSPACE pWorkspace);

    // When slave closes or nothing pinned, promote from hidden queue
    void promoteFromQueue(int wsID);

    // Move a window to/from the hidden special workspace
    void hideWindow(PHLWINDOW pWindow);
    void unhideWindow(PHLWINDOW pWindow, int targetWsID);
};

inline CTwoPanePersistentLayout* g_pTPPLayout = nullptr;
inline HANDLE PHANDLE = nullptr;
