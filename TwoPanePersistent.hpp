#pragma once

#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/target/WindowTarget.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <deque>
#include <vector>
#include <optional>

using namespace Layout;

// ── Per-workspace state ───────────────────────────────────────────────────────
struct STPPState {
    // The pinned slave: the last focused non-master target.
    // When focus returns to master, this stays unchanged (the "persistent" part).
    WP<ITarget> pinnedSlave;

    // Hidden targets in cycle order. Front = next to swap in.
    std::deque<WP<ITarget>> hiddenQueue;

    // Master/slave split fraction (master takes this fraction of width)
    float mfact = 0.55f;
};

// ── Layout algorithm ──────────────────────────────────────────────────────────
class CTPPAlgorithm : public ITiledAlgorithm {
  public:
    CTPPAlgorithm()          = default;
    virtual ~CTPPAlgorithm() = default;

    // ── ITiledAlgorithm interface ─────────────────────────────────────────
    virtual void newTarget(SP<ITarget> target) override;
    virtual void movedTarget(SP<ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt) override;
    virtual void removeTarget(SP<ITarget> target) override;
    virtual void resizeTarget(const Vector2D& delta, SP<ITarget> target, eRectCorner corner = CORNER_NONE) override;
    virtual void recalculate() override;
    virtual SP<ITarget> getNextCandidate(SP<ITarget> old) override;
    virtual std::expected<void, std::string> layoutMsg(const std::string_view& sv) override;
    virtual std::optional<Vector2D> predictSizeForNewTarget() override;
    virtual void swapTargets(SP<ITarget> a, SP<ITarget> b) override;
    virtual void moveTargetInDirection(SP<ITarget> t, Math::eDirection dir, bool silent) override;

    // ── Called by focus hook ──────────────────────────────────────────────
    void onTargetFocused(SP<ITarget> target);

  private:
    // All visible tiled targets (master + current slave only)
    std::vector<WP<ITarget>> m_targets;

    // Per-workspace state (keyed by workspace ID)
    std::unordered_map<WORKSPACEID, STPPState> m_wsStates;

    STPPState& stateFor(SP<ITarget> t);
    STPPState& stateForWs(WORKSPACEID id);

    SP<ITarget> getMaster(WORKSPACEID wsID);
    SP<ITarget> getPinnedSlave(WORKSPACEID wsID);

    bool isMaster(SP<ITarget> t);

    void recalculateForSpace(SP<CSpace> space);
    void cycleNext(WORKSPACEID wsID);
    void cyclePrev(WORKSPACEID wsID);
    void focusMaster(WORKSPACEID wsID);
    void focusSlave(WORKSPACEID wsID);

    void promoteFromQueue(WORKSPACEID wsID);
    void hideTarget(SP<ITarget> t);
    void unhideTarget(SP<ITarget> t);
};

inline CTPPAlgorithm* g_pTPPAlgo = nullptr;
inline HANDLE         PHANDLE    = nullptr;