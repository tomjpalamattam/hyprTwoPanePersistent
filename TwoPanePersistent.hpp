#pragma once

#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/Workspace.hpp>

#include <deque>
#include <vector>
#include <optional>
#include <unordered_map>

using namespace Layout;

struct STPPState {
    WP<ITarget>             pinnedSlave;
    std::deque<WP<ITarget>> hiddenQueue;
    float                   mfact = 0.55f;
};

class CTPPAlgorithm : public ITiledAlgorithm {
  public:
    CTPPAlgorithm()          = default;
    virtual ~CTPPAlgorithm() = default;

    virtual void        newTarget(SP<ITarget> target) override;
    virtual void        movedTarget(SP<ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt) override;
    virtual void        removeTarget(SP<ITarget> target) override;
    virtual void        resizeTarget(const Vector2D& delta, SP<ITarget> target, eRectCorner corner = CORNER_NONE) override;
    virtual void        recalculate() override;
    virtual SP<ITarget> getNextCandidate(SP<ITarget> old) override;
    virtual std::expected<void, std::string> layoutMsg(const std::string_view& sv) override;
    virtual std::optional<Vector2D>          predictSizeForNewTarget() override;
    virtual void        swapTargets(SP<ITarget> a, SP<ITarget> b) override;
    virtual void        moveTargetInDirection(SP<ITarget> t, Math::eDirection dir, bool silent) override;

    void onTargetFocused(SP<ITarget> target);

  private:
    std::vector<WP<ITarget>>                   m_targets;
    std::unordered_map<WORKSPACEID, STPPState> m_wsStates;

    STPPState&  stateForWs(WORKSPACEID id);
    WORKSPACEID wsIDOf(SP<ITarget> t);

    SP<ITarget> getMaster(WORKSPACEID wsID);
    SP<ITarget> getPinnedSlave(WORKSPACEID wsID);
    bool        isMaster(SP<ITarget> t);
    SP<CSpace>  getSpace();

    void recalculateForSpace(SP<CSpace> space, WORKSPACEID wsID);
    void cycleNext(WORKSPACEID wsID);
    void cyclePrev(WORKSPACEID wsID);
    void promoteFromQueue(WORKSPACEID wsID);
    void hideTarget(SP<ITarget> t, SP<CSpace> space);
    void unhideTarget(SP<ITarget> t, SP<CSpace> space);
};

inline CTPPAlgorithm* g_pTPPAlgo = nullptr;
inline HANDLE         PHANDLE    = nullptr;