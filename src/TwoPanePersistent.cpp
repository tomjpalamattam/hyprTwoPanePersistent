#include "TwoPanePersistent.hpp"
#include "globals.hpp"

#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/target/WindowGroupTarget.hpp>

#include <hyprland/src/desktop/Workspace.hpp>

// Hyprland main split CWindow apart: Window.hpp moved under view/window/ and the
// presentation-ish methods moved behind a WindowPresentation sub-object. 0.56.x
// has one flat view/Window.hpp with those methods directly on CWindow.
#if __has_include(<hyprland/src/desktop/view/window/Window.hpp>)
#define TPP_HYPRLAND_SPLIT_WINDOW 1
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#else
#define TPP_HYPRLAND_SPLIT_WINDOW 0
#include <hyprland/src/desktop/view/Window.hpp>
#endif

#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>

#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/fullscreen/handler/FullscreenHandler.hpp>

#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>

#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/event/EventBus.hpp>

#include <hyprland/src/config/ConfigValue.hpp>

#include <hyprutils/string/VarList2.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

using namespace Hyprutils::String;
using namespace Layout;
using namespace Layout::Tiled;

static constexpr float MIN_SPLIT = 0.05F;
static constexpr float MAX_SPLIT = 0.95F;

CTwoPanePersistent::CTwoPanePersistent() {
    // Focusing a stack window makes it the slave. Focusing the master changes
    // nothing -- that is the "persistent" half of the name.
    m_focusCallback = Event::bus()->m_events.window.active.listen([this](PHLWINDOW pWindow, Desktop::eFocusReason reason) {
        if (!pWindow || !pWindow->m_workspace || !pWindow->m_workspace->isVisible())
            return;

        const auto TARGET = pWindow->layoutTarget();
        if (!TARGET)
            return;

        onFocusChanged(TARGET);
    });
}

CTwoPanePersistent::~CTwoPanePersistent() {
    unhideAll();
    m_focusCallback.reset();
}

std::optional<std::string> CTwoPanePersistent::layoutName() const {
    return "twopanepersistent";
}

//
// ---- bookkeeping -----------------------------------------------------------
//

SP<STPPNode> CTwoPanePersistent::nodeFor(SP<ITarget> t) const {
    for (const auto& n : m_nodes) {
        if (n->target.lock() == t)
            return n;
    }
    return nullptr;
}

int CTwoPanePersistent::indexOf(SP<ITarget> t) const {
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i]->target.lock() == t)
            return static_cast<int>(i);
    }
    return -1;
}

SP<ITarget> CTwoPanePersistent::masterTarget() const {
    return m_nodes.empty() ? nullptr : m_nodes[0]->target.lock();
}

void CTwoPanePersistent::pruneDead() {
    std::erase_if(m_nodes, [](const auto& n) { return !n || !n->target || !n->target.lock(); });
}

float CTwoPanePersistent::configuredSplit() const {
    if (g_pTPPState && g_pTPPState->config.mfact)
        return std::clamp(static_cast<float>(g_pTPPState->config.mfact->value()), MIN_SPLIT, MAX_SPLIT);
    return 0.5F;
}

// This is the direct translation of focusedMaster's slave selection:
//
//   Just win -> if win `elem` down s && focus s /= win then win else nextSlave
//   Nothing  -> nextSlave
//
SP<ITarget> CTwoPanePersistent::resolveSlave() {
    if (m_nodes.size() < 2)
        return nullptr;

    const auto MASTER = masterTarget();
    const auto CACHED = m_slave.lock();

    if (CACHED && CACHED != MASTER && indexOf(CACHED) > 0)
        return CACHED;

    // No usable remembered slave: fall back to the window directly below master.
    const auto NEXT = m_nodes[1]->target.lock();
    m_slave         = NEXT;
    return NEXT;
}

//
// ---- visibility ------------------------------------------------------------
//

// Hiding uses an animated alpha channel plus an input block, the same shape
// CMonocleAlgorithm uses.
//
// setHidden() also works and is simpler, but it unmaps and suspends the window,
// so a revealed window pops in with no transition. Alpha channels are
// PHLANIMVAR<float> -- assigning them plays Hyprland's real fade and respects
// the user's animation config.
//
// The channel choice matters. Groups own WINDOW_ALPHA_LAYOUT and rewrite it on
// every tab switch (CGroup::updateWindowVisibility: active 1.F, inactive 0.F),
// so a layout writing that channel loses the race and hidden grouped windows
// pop back into view on Tab. WINDOW_ALPHA_MOVE_FROM_WORKSPACE has no writers in
// 0.56.x. Channels multiply (SMultiplyOperation, identity 1.0), so holding 0
// here forces the window invisible regardless of what the group does to LAYOUT.

// A group presents to the layout as a single ITarget whose window() is only the
// *current* tab, so hiding that alone would leave the other members visible.
// Collect every window backing this target instead.
std::vector<PHLWINDOW> CTwoPanePersistent::windowsFor(SP<ITarget> t) const {
    std::vector<PHLWINDOW> out;

    if (!t)
        return out;

    if (t->type() == TARGET_TYPE_GROUP) {
        const auto GROUP_TARGET = dynamicPointerCast<CWindowGroupTarget>(t);
        if (GROUP_TARGET) {
            const auto GROUP = GROUP_TARGET->getGroup();
            if (GROUP) {
                for (const auto& wref : GROUP->windows()) {
                    const auto W = wref.lock();
                    if (W)
                        out.emplace_back(W);
                }
                return out;
            }
        }
    }

    const auto WINDOW = t->window();
    if (WINDOW)
        out.emplace_back(WINDOW);

    return out;
}

void CTwoPanePersistent::setWindowHidden(SP<ITarget> t, bool hidden) const {
    for (const auto& WINDOW : windowsFor(t)) {
        // Clear any leftover setHidden state from an earlier build of this
        // plugin. Without this, a window left unmapped stays unmapped forever
        // and the alpha below can never make it visible again.
        if (WINDOW->isHidden())
            WINDOW->setHidden(false);

#if TPP_HYPRLAND_SPLIT_WINDOW
        WINDOW->setInputBlocked(Desktop::View::FOCUS_BLOCK_MONOCLE_INACTIVE, hidden);
        *WINDOW->presentation().alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE) = hidden ? 0.F : 1.F;
#else
        WINDOW->setInputBlocked(Desktop::View::INPUT_BLOCK_MONOCLE_INACTIVE, hidden);
        *WINDOW->alpha(Desktop::View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE) = hidden ? 0.F : 1.F;
#endif
    }
}

void CTwoPanePersistent::unhideAll() const {
    for (const auto& n : m_nodes) {
        setWindowHidden(n->target.lock(), false);
    }
}

//
// ---- ITiledAlgorithm -------------------------------------------------------
//

void CTwoPanePersistent::newTarget(SP<ITarget> target) {
    if (!target || nodeFor(target))
        return;

    if (!m_splitSeeded) {
        m_split       = configuredSplit();
        m_splitSeeded = true;
    }

    // A tiled window dropped by mouse arrives here as a fresh target, because
    // dragBegin() floated it and that ran removeTarget(). wasDraggingWindow() is
    // still true at this point (dragEnd clears it only after re-tiling), so this
    // is where the drop position has to be honoured -- CMasterAlgorithm does the
    // same thing in addTarget().
    if (!m_nodes.empty() && m_parent && m_parent->space() && g_layoutManager && g_layoutManager->dragController()->wasDraggingWindow()) {
        const auto AREA   = m_parent->space()->workArea();
        const auto COORDS = g_pInputManager ? g_pInputManager->getMouseCoordsInternal() : AREA.middle();

        // Dropped on the master side: take the master pane, old master drops
        // into the slave pane so the visible pair stays the visible pair.
        if (COORDS.x < AREA.x + AREA.w * m_split) {
            const auto OLD_MASTER = masterTarget();
            m_nodes.insert(m_nodes.begin(), makeShared<STPPNode>(target));
            m_slave = OLD_MASTER;
            recalculate();
            return;
        }

        // Dropped on the slave side: normal insert below master, becomes slave.
        m_nodes.insert(m_nodes.begin() + 1, makeShared<STPPNode>(target));
        m_slave = target;
        recalculate();
        return;
    }

    // New windows go directly below the master, matching XMonad's insertion into
    // the stack just after the focused window when master has focus.
    const auto FOCUSED = focusedTarget();
    const auto MASTER  = masterTarget();

    if (!m_nodes.empty() && FOCUSED && FOCUSED != MASTER) {
        const int IDX = indexOf(FOCUSED);
        if (IDX > 0) {
            m_nodes.insert(m_nodes.begin() + IDX + 1, makeShared<STPPNode>(target));
            m_slave = target;
            recalculate();
            return;
        }
    }

    if (m_nodes.empty())
        m_nodes.emplace_back(makeShared<STPPNode>(target));
    else {
        m_nodes.insert(m_nodes.begin() + 1, makeShared<STPPNode>(target));
        m_slave = target;
    }

    recalculate();
}

// A tiled drag-drop lands in newTarget(), not here: dragBegin floats the window,
// which runs CAlgorithm::setFloating -> removeTarget(), so by drop time the
// target has already left m_nodes. See the drag handling in newTarget().
void CTwoPanePersistent::movedTarget(SP<ITarget> target, std::optional<Vector2D> focalPoint) {
    newTarget(target);
}

void CTwoPanePersistent::removeTarget(SP<ITarget> target) {
    const int IDX = indexOf(target);
    if (IDX < 0)
        return;

    // Always un-hide on the way out, otherwise the window stays invisible in
    // whatever layout it lands in next.
    setWindowHidden(target, false);

    const bool WAS_SLAVE = (m_slave.lock() == target);
    m_nodes.erase(m_nodes.begin() + IDX);

    if (WAS_SLAVE)
        m_slave.reset();

    pruneDead();

    if (m_nodes.empty()) {
        m_slave.reset();
        return;
    }

    recalculate();
}

void CTwoPanePersistent::resizeTarget(const Vector2D& delta, SP<ITarget> target, eRectCorner corner) {
    if (!m_parent || !m_parent->space() || m_nodes.size() < 2)
        return;

    const auto AREA = m_parent->space()->workArea();
    if (AREA.w <= 1.0)
        return;

    // Dragging the master's right edge and the slave's left edge both mean
    // "move the divider", so flip the sign for the slave side.
    const bool  IS_MASTER = (target == masterTarget());
    const float DELTA     = static_cast<float>(delta.x / AREA.w) * (IS_MASTER ? 1.F : -1.F);

    m_split       = std::clamp(m_split + DELTA, MIN_SPLIT, MAX_SPLIT);
    m_splitSeeded = true;

    recalculate();
}

void CTwoPanePersistent::recalculate(eRecalculateReason reason) {
    pruneDead();

    if (m_nodes.empty() || !m_parent || !m_parent->space())
        return;

    if (Fullscreen::controller()->hasFullscreen(m_parent->space()->workspace(), true)) {
        m_defaultFullscreenHandler->syncTargetSizeAndPosition();
        return;
    }

    if (!m_splitSeeded) {
        m_split       = configuredSplit();
        m_splitSeeded = true;
    }

    const auto MASTER = masterTarget();
    const auto SLAVE  = resolveSlave();

    // Visibility first, so workspace rules that count visible windows (w[tv1] and
    // friends) see the right number before we ask for the work area.
    for (const auto& n : m_nodes) {
        const auto TARGET = n->target.lock();
        if (!TARGET)
            continue;

        setWindowHidden(TARGET, TARGET != MASTER && TARGET != SLAVE);
    }

    m_parent->space()->recheckWorkArea();
    const auto AREA = m_parent->space()->workArea();

    // Master alone gets the whole area, exactly like the [] branch of focusedMaster.
    if (!SLAVE) {
        if (MASTER)
            MASTER->setPositionGlobal(AREA);
        return;
    }

    const CBox LEFT{AREA.x, AREA.y, AREA.w * m_split, AREA.h};
    const CBox RIGHT{AREA.x + AREA.w * m_split, AREA.y, AREA.w * (1.F - m_split), AREA.h};

    if (MASTER)
        MASTER->setPositionGlobal(LEFT.copy().noNegativeSize());

    // Hidden windows are parked in the slave pane so they are already the right
    // size the moment they get cycled into view.
    for (const auto& n : m_nodes) {
        const auto TARGET = n->target.lock();
        if (!TARGET || TARGET == MASTER)
            continue;

        TARGET->setPositionGlobal(RIGHT.copy().noNegativeSize());
    }
}

SP<ITarget> CTwoPanePersistent::getNextCandidate(SP<ITarget> old) {
    pruneDead();

    if (m_nodes.empty())
        return nullptr;

    const int IDX = indexOf(old);
    if (IDX < 0) {
        const auto SLAVE = m_slave.lock();
        return SLAVE ? SLAVE : masterTarget();
    }

    if (m_nodes.size() == 1)
        return nullptr;

    return m_nodes[(IDX + 1) % m_nodes.size()]->target.lock();
}

std::optional<Vector2D> CTwoPanePersistent::predictSizeForNewTarget() {
    if (!m_parent || !m_parent->space())
        return std::nullopt;

    const auto AREA = m_parent->space()->workArea();

    if (m_nodes.empty())
        return AREA.size();

    return Vector2D{AREA.w * (1.F - m_split), AREA.h};
}

void CTwoPanePersistent::swapTargets(SP<ITarget> a, SP<ITarget> b) {
    const int IA = indexOf(a);
    const int IB = indexOf(b);

    // Grouping does NOT go through removeTarget/newTarget. CGroup::init() calls
    // LayoutManager::switchTargets, which lands here with the *new* group target
    // as one side and an existing window target as the other. So a one-sided
    // match is a replacement, not a no-op -- bailing here would leave a stale
    // target in m_nodes that we keep positioning and hiding forever.
    if (IA < 0 && IB < 0)
        return;

    if (IA >= 0 && IB >= 0)
        std::swap(m_nodes[IA]->target, m_nodes[IB]->target);
    else if (IA >= 0) {
        // a is ours, b is replacing it
        setWindowHidden(a, false);
        m_nodes[IA]->target = b;
    } else {
        // b is ours, a is replacing it
        setWindowHidden(b, false);
        m_nodes[IB]->target = a;
    }

    // If one of them was the remembered slave, the remembered slave should follow
    // the pane, not the window.
    const auto SLAVE = m_slave.lock();
    if (SLAVE == a)
        m_slave = b;
    else if (SLAVE == b)
        m_slave = a;

    recalculate();
}

void CTwoPanePersistent::moveTargetInDirection(SP<ITarget> t, Math::eDirection dir, bool silent) {
    const int IDX = indexOf(t);
    if (IDX < 0)
        return;

    const bool BACKWARD = (dir == Math::DIRECTION_LEFT || dir == Math::DIRECTION_UP);
    const bool FORWARD  = (dir == Math::DIRECTION_RIGHT || dir == Math::DIRECTION_DOWN);

    if (!BACKWARD && !FORWARD)
        return;

    const bool AT_EDGE = (BACKWARD && IDX == 0) || (FORWARD && IDX + 1 >= static_cast<int>(m_nodes.size()));

    // At the end of the stack, hand off to the monitor in that direction if the
    // user has that enabled, same as the built-in layouts do.
    if (AT_EDGE) {
        static auto PMONITORFALLBACK = CConfigValue<Config::INTEGER>("binds:window_direction_monitor_fallback");

        if (!*PMONITORFALLBACK || !t->space() || !t->space()->workspace())
            return;

        const auto PMONITOR  = t->space()->workspace()->m_monitor.lock();
        const auto PMONINDIR = State::monitorState()->query().relativeTo(PMONITOR).inDirection(dir).run();

        if (PMONINDIR && PMONINDIR != PMONITOR) {
            if (t->window()) {
#if TPP_HYPRLAND_SPLIT_WINDOW
                t->window()->presentation().setAnimationsToMove();
#else
                t->window()->setAnimationsToMove();
#endif
            }

            t->assignToSpace(PMONINDIR->m_activeWorkspace->m_space, focalPointForDir(t, dir));
        }

        return;
    }

    std::swap(m_nodes[IDX], m_nodes[BACKWARD ? IDX - 1 : IDX + 1]);

    // Whatever we just moved should be what you are looking at.
    if (indexOf(t) > 0)
        m_slave = t;

    recalculate();
}

//
// ---- focus + cycling -------------------------------------------------------
//

SP<ITarget> CTwoPanePersistent::focusedTarget() const {
    const auto WINDOW = Desktop::focusState()->window();
    if (!WINDOW)
        return nullptr;

    const auto TARGET = WINDOW->layoutTarget();
    if (!TARGET || indexOf(TARGET) < 0)
        return nullptr;

    return TARGET;
}

void CTwoPanePersistent::focusTarget(SP<ITarget> t) const {
    if (!t)
        return;

    const auto WINDOW = t->window();
    if (!WINDOW)
        return;

    Desktop::focusState()->fullWindowFocus(WINDOW, Desktop::FOCUS_REASON_KEYBIND);
}

void CTwoPanePersistent::onFocusChanged(SP<ITarget> t) {
    const int IDX = indexOf(t);

    // Not ours, or it's the master: leave the slave pane exactly as it is.
    if (IDX <= 0)
        return;

    if (m_slave.lock() == t)
        return;

    m_slave = t;
    recalculate();
}

// XMonad's Tab is focusDown over the whole stack, master included. Focusing a
// stack window promotes it into the slave pane via onFocusChanged.
//
// NOTE: hl.dsp.window.cycle_next() cannot drive this. Actions::cycleNext gates
// on a hardcoded typeid whitelist (CMonocleAlgorithm, CMasterAlgorithm) with no
// registration hook, and its fallback query filters on acceptsInput(), which
// input-blocked stack windows fail. Bind hl.dsp.layout("cyclenext") instead --
// that is the same call cycle_next makes internally for monocle.
void CTwoPanePersistent::cycleFocus(int delta) {
    pruneDead();

    if (m_nodes.size() < 2)
        return;

    const auto FOCUSED = focusedTarget();
    const int  N       = static_cast<int>(m_nodes.size());

    int idx = FOCUSED ? indexOf(FOCUSED) : 0;
    idx     = ((idx + delta) % N + N) % N;

    focusTarget(m_nodes[idx]->target.lock());
}

void CTwoPanePersistent::promoteToMaster(SP<ITarget> t) {
    const int IDX = indexOf(t);
    if (IDX <= 0)
        return;

    const auto OLD_MASTER = masterTarget();

    auto       node = m_nodes[IDX];
    m_nodes.erase(m_nodes.begin() + IDX);
    m_nodes.insert(m_nodes.begin(), node);

    // The window that just left the master pane becomes the slave, so the pair
    // on screen stays the same pair -- just swapped.
    m_slave = OLD_MASTER;

    recalculate();
}

//
// ---- layoutmsg -------------------------------------------------------------
//

Config::ErrorResult CTwoPanePersistent::layoutMsg(const std::string_view& sv) {
    CVarList2  vars(std::string{sv}, 0, 's');

    const auto badArg = [](const std::string& msg) {
        return Config::configError(msg, Config::eConfigErrorLevel::ERROR, Config::eConfigErrorCode::INVALID_ARGUMENT);
    };

    if (vars.size() < 1)
        return badArg("twopanepersistent: layoutmsg requires at least 1 argument");

    const auto COMMAND = vars[0];

    if (COMMAND == "cyclenext") {
        cycleFocus(1);
        return {};
    }

    if (COMMAND == "cycleprev") {
        cycleFocus(-1);
        return {};
    }

    if (COMMAND == "focusmaster") {
        focusTarget(masterTarget());
        return {};
    }

    if (COMMAND == "swapwithmaster") {
        const auto FOCUSED = focusedTarget();
        if (!FOCUSED)
            return {};

        if (FOCUSED == masterTarget()) {
            // Already on master: swap with whatever is currently in the slave pane.
            promoteToMaster(resolveSlave());
        } else
            promoteToMaster(FOCUSED);

        focusTarget(masterTarget());
        return {};
    }

    if (COMMAND == "mfact") {
        float step = 0.03F;
        if (g_pTPPState && g_pTPPState->config.dfact)
            step = static_cast<float>(g_pTPPState->config.dfact->value());

        if (vars.size() < 2) {
            m_split = configuredSplit();
        } else {
            const std::string ARG{vars[1]};

            if (ARG == "+")
                m_split += step;
            else if (ARG == "-")
                m_split -= step;
            else {
                try {
                    const float VAL = std::stof(ARG);
                    // A leading sign means relative, a bare number means absolute.
                    if (ARG.starts_with('+') || ARG.starts_with('-'))
                        m_split += VAL;
                    else
                        m_split = VAL;
                } catch (...) { return badArg(std::format("twopanepersistent: mfact expects +, -, or a number, got {}", ARG)); }
            }
        }

        m_split       = std::clamp(m_split, MIN_SPLIT, MAX_SPLIT);
        m_splitSeeded = true;

        recalculate();
        return {};
    }

    return badArg(std::format("twopanepersistent: unknown layoutmsg {}. Expected cyclenext, cycleprev, focusmaster, swapwithmaster, or mfact", COMMAND));
}