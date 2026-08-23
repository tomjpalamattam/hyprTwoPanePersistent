#pragma once

#include <hyprland/src/layout/algorithm/TiledAlgorithm.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Layout::Tiled {

    // One node per tiled target on this space, kept in stack order.
    // m_nodes[0] is the master; everything else is the stack.
    struct STPPNode {
        STPPNode(SP<ITarget> t) : target(t) {}
        WP<ITarget> target;
    };

    class CTwoPanePersistent : public ITiledAlgorithm {
      public:
        CTwoPanePersistent();
        virtual ~CTwoPanePersistent();

        virtual void                       newTarget(SP<ITarget> target);
        virtual void                       movedTarget(SP<ITarget> target, std::optional<Vector2D> focalPoint = std::nullopt);
        virtual void                       removeTarget(SP<ITarget> target);

        virtual void                       resizeTarget(const Vector2D& delta, SP<ITarget> target, eRectCorner corner = CORNER_NONE);
        virtual void                       recalculate(eRecalculateReason reason = RECALCULATE_REASON_UNKNOWN);

        virtual SP<ITarget>                getNextCandidate(SP<ITarget> old);

        virtual Config::ErrorResult        layoutMsg(const std::string_view& sv);
        virtual std::optional<Vector2D>    predictSizeForNewTarget();

        virtual void                       swapTargets(SP<ITarget> a, SP<ITarget> b);
        virtual void                       moveTargetInDirection(SP<ITarget> t, Math::eDirection dir, bool silent);

        virtual std::optional<std::string> layoutName() const;

      private:
        std::vector<SP<STPPNode>> m_nodes;
        CHyprSignalListener       m_focusCallback;

        // The remembered slave. This is the whole point of the layout: it does not
        // change when focus returns to the master.
        WP<ITarget> m_slave;

        // Master pane fraction. Seeded from config, then owned by the instance so
        // that resizes and `mfact` are per-workspace.
        float m_split       = 0.5F;
        bool  m_splitSeeded = false;

        SP<STPPNode> nodeFor(SP<ITarget> t) const;
        int          indexOf(SP<ITarget> t) const;

        // A group target backs several windows; hiding needs all of them.
        std::vector<PHLWINDOW> windowsFor(SP<ITarget> t) const;

        SP<ITarget>  masterTarget() const;
        // Resolves (and caches) which target belongs in the right pane.
        SP<ITarget>  resolveSlave();

        void         pruneDead();
        void         setWindowHidden(SP<ITarget> t, bool hidden) const;
        void         unhideAll() const;

        SP<ITarget>  focusedTarget() const;
        void         cycleFocus(int delta);
        void         focusTarget(SP<ITarget> t) const;

        void         promoteToMaster(SP<ITarget> t);
        void         onFocusChanged(SP<ITarget> t);

        float        configuredSplit() const;
    };
};
