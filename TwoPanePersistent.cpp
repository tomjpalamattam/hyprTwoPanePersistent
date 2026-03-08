#include "TwoPanePersistent.hpp"

// ── State helpers ─────────────────────────────────────────────────────────────

STPPState& CTPPAlgorithm::stateFor(SP<ITarget> t) {
    auto ws = t->workspace();
    return stateForWs(ws ? ws->m_iID : WORKSPACE_INVALID);
}

STPPState& CTPPAlgorithm::stateForWs(WORKSPACEID id) {
    return m_wsStates[id]; // default-constructs if missing
}

SP<ITarget> CTPPAlgorithm::getMaster(WORKSPACEID wsID) {
    // Master = first target added to this workspace (index 0 in insertion order)
    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (!t) continue;
        auto ws = t->workspace();
        if (ws && ws->m_iID == wsID)
            return t;
    }
    return nullptr;
}

SP<ITarget> CTPPAlgorithm::getPinnedSlave(WORKSPACEID wsID) {
    auto& st = stateForWs(wsID);
    auto  t  = st.pinnedSlave.lock();
    if (!t) return nullptr;
    // Validate it's still a live target in our list
    for (auto& wt : m_targets) {
        if (wt.lock() == t)
            return t;
    }
    st.pinnedSlave.reset();
    return nullptr;
}

bool CTPPAlgorithm::isMaster(SP<ITarget> t) {
    auto ws = t->workspace();
    if (!ws) return false;
    return getMaster(ws->m_iID) == t;
}

// ── Hide/unhide via space ghost ───────────────────────────────────────────────
// We use setSpaceGhost to keep the target in the space but exclude it from layout,
// effectively hiding it without moving it to a different workspace.

void CTPPAlgorithm::hideTarget(SP<ITarget> t) {
    if (!t) return;
    auto sp = m_parent.lock() ? m_parent.lock()->space() : nullptr;
    if (sp) t->setSpaceGhost(sp);
}

void CTPPAlgorithm::unhideTarget(SP<ITarget> t) {
    if (!t) return;
    auto sp = m_parent.lock() ? m_parent.lock()->space() : nullptr;
    if (sp) t->assignToSpace(sp);
}

// ── ITiledAlgorithm: window lifecycle ─────────────────────────────────────────

void CTPPAlgorithm::newTarget(SP<ITarget> target) {
    auto ws = target->workspace();
    if (!ws) return;

    WORKSPACEID wsID = ws->m_iID;
    auto&       st   = stateForWs(wsID);

    SP<ITarget> master = getMaster(wsID);
    SP<ITarget> slave  = getPinnedSlave(wsID);

    if (!master) {
        // First window → master
        m_targets.push_back(target);
    } else if (!slave) {
        // Second window → pinned slave
        m_targets.push_back(target);
        st.pinnedSlave = target;
    } else {
        // Third+ → goes into hidden queue
        m_targets.push_back(target);
        st.hiddenQueue.push_back(target);
        hideTarget(target);
        return; // don't recalculate, window is hidden
    }

    recalculate();
}

void CTPPAlgorithm::movedTarget(SP<ITarget> target, std::optional<Vector2D>) {
    // Target moved from another space/algorithm into ours — treat like new
    newTarget(target);
}

void CTPPAlgorithm::removeTarget(SP<ITarget> target) {
    auto ws = target->workspace();
    WORKSPACEID wsID = ws ? ws->m_iID : WORKSPACE_INVALID;
    auto& st = stateForWs(wsID);

    bool wasMaster = isMaster(target);
    bool wasSlave  = (getPinnedSlave(wsID) == target);

    // Remove from our target list
    m_targets.erase(std::remove_if(m_targets.begin(), m_targets.end(),
        [&](const WP<ITarget>& wt) { return wt.lock() == target; }), m_targets.end());

    // Remove from hidden queue if present
    st.hiddenQueue.erase(std::remove_if(st.hiddenQueue.begin(), st.hiddenQueue.end(),
        [&](const WP<ITarget>& wt) { return wt.lock() == target; }), st.hiddenQueue.end());

    if (wasMaster) {
        // Promote current slave to master (it becomes the new index-0 for this ws)
        // The slave is already in m_targets; getMaster() returns first entry for ws,
        // so we just need to make sure slave comes first — swap it to front.
        auto slaveTarget = getPinnedSlave(wsID);
        if (slaveTarget) {
            // Move slave to front of m_targets for this workspace
            auto it = std::find_if(m_targets.begin(), m_targets.end(),
                [&](const WP<ITarget>& wt) { return wt.lock() == slaveTarget; });
            if (it != m_targets.end()) {
                std::rotate(m_targets.begin(), it, it + 1);
            }
        }
        st.pinnedSlave.reset();
        // Promote from queue to fill slave slot
        promoteFromQueue(wsID);
    } else if (wasSlave) {
        st.pinnedSlave.reset();
        promoteFromQueue(wsID);
    }

    recalculate();
}

// ── Geometry ──────────────────────────────────────────────────────────────────

void CTPPAlgorithm::recalculate() {
    auto parent = m_parent.lock();
    if (!parent) return;
    auto space = parent->space();
    if (!space) return;
    recalculateForSpace(space);
}

void CTPPAlgorithm::recalculateForSpace(SP<CSpace> space) {
    if (!space) return;

    auto ws = space->workspace();
    if (!ws) return;

    WORKSPACEID wsID = ws->m_iID;
    auto& st = stateForWs(wsID);

    SP<ITarget> master = getMaster(wsID);
    SP<ITarget> slave  = getPinnedSlave(wsID);

    const CBox& area = space->workArea();

    if (!master) return;

    if (!slave) {
        // Only master — full area
        master->setPositionGlobal(area);
        master->warpPositionSize();
        return;
    }

    // Two pane: master left, slave right
    float masterW = area.w * st.mfact;
    float slaveW  = area.w - masterW;

    CBox masterBox = {area.x, area.y, masterW, area.h};
    CBox slaveBox  = {area.x + masterW, area.y, slaveW, area.h};

    master->setPositionGlobal(masterBox);
    master->warpPositionSize();

    slave->setPositionGlobal(slaveBox);
    slave->warpPositionSize();
}

void CTPPAlgorithm::resizeTarget(const Vector2D& delta, SP<ITarget> target, eRectCorner corner) {
    auto ws = target->workspace();
    if (!ws) return;

    auto& st   = stateForWs(ws->m_iID);
    auto  space = m_parent.lock() ? m_parent.lock()->space() : nullptr;
    if (!space) return;

    float totalW = space->workArea().w;
    if (totalW <= 0) return;

    if (isMaster(target))
        st.mfact += delta.x / totalW;
    else
        st.mfact -= delta.x / totalW;

    st.mfact = std::clamp(st.mfact, 0.1f, 0.9f);
    recalculate();
}

// ── Cycle and layout messages ─────────────────────────────────────────────────

void CTPPAlgorithm::promoteFromQueue(WORKSPACEID wsID) {
    auto& st = stateForWs(wsID);
    while (!st.hiddenQueue.empty()) {
        auto next = st.hiddenQueue.front().lock();
        st.hiddenQueue.pop_front();
        if (next) {
            unhideTarget(next);
            st.pinnedSlave = next;
            return;
        }
    }
}

void CTPPAlgorithm::cycleNext(WORKSPACEID wsID) {
    auto& st = stateForWs(wsID);
    if (st.hiddenQueue.empty()) return;

    SP<ITarget> currentSlave = getPinnedSlave(wsID);

    // Pop next from front
    SP<ITarget> next;
    while (!st.hiddenQueue.empty()) {
        next = st.hiddenQueue.front().lock();
        st.hiddenQueue.pop_front();
        if (next) break;
        next = nullptr;
    }
    if (!next) return;

    // Send current slave to back of queue and hide it
    if (currentSlave) {
        hideTarget(currentSlave);
        st.hiddenQueue.push_back(currentSlave);
    }

    unhideTarget(next);
    st.pinnedSlave = next;
    recalculate();
}

void CTPPAlgorithm::cyclePrev(WORKSPACEID wsID) {
    auto& st = stateForWs(wsID);
    if (st.hiddenQueue.empty()) return;

    SP<ITarget> currentSlave = getPinnedSlave(wsID);

    SP<ITarget> prev;
    while (!st.hiddenQueue.empty()) {
        prev = st.hiddenQueue.back().lock();
        st.hiddenQueue.pop_back();
        if (prev) break;
        prev = nullptr;
    }
    if (!prev) return;

    if (currentSlave) {
        hideTarget(currentSlave);
        st.hiddenQueue.push_front(currentSlave);
    }

    unhideTarget(prev);
    st.pinnedSlave = prev;
    recalculate();
}

std::expected<void, std::string> CTPPAlgorithm::layoutMsg(const std::string_view& sv) {
    auto parent = m_parent.lock();
    if (!parent) return {};
    auto space = parent->space();
    if (!space) return {};
    auto ws = space->workspace();
    if (!ws) return {};

    WORKSPACEID wsID = ws->m_iID;

    if (sv == "cyclenext")
        cycleNext(wsID);
    else if (sv == "cycleprev")
        cyclePrev(wsID);
    else if (sv == "focusmaster") {
        auto master = getMaster(wsID);
        if (master && master->window())
            g_pCompositor->focusWindow(master->window());
    } else if (sv == "focusslave") {
        auto slave = getPinnedSlave(wsID);
        if (slave && slave->window())
            g_pCompositor->focusWindow(slave->window());
    } else if (sv.starts_with("mfact ")) {
        try {
            float val = std::stof(std::string(sv.substr(6)));
            stateForWs(wsID).mfact = std::clamp(val, 0.1f, 0.9f);
            recalculate();
        } catch (...) {}
    } else {
        return std::unexpected("Unknown message: " + std::string(sv));
    }

    return {};
}

// ── Focus hook ────────────────────────────────────────────────────────────────
// Core of TwoPanePersistent: slave focused → update pin; master focused → do nothing.

void CTPPAlgorithm::onTargetFocused(SP<ITarget> target) {
    if (!target) return;
    auto ws = target->workspace();
    if (!ws) return;

    WORKSPACEID wsID = ws->m_iID;

    if (isMaster(target))
        return; // master focused — persistence: slave stays pinned

    // A non-master was focused — check it's actually one of our visible targets
    bool found = false;
    for (auto& wt : m_targets) {
        if (wt.lock() == target) { found = true; break; }
    }
    if (!found) return;

    auto& st = stateForWs(wsID);
    if (st.pinnedSlave.lock() == target) return; // already pinned, no change

    st.pinnedSlave = target;
    recalculate();
}

// ── Remaining interface stubs ─────────────────────────────────────────────────

SP<ITarget> CTPPAlgorithm::getNextCandidate(SP<ITarget> old) {
    if (!old) return nullptr;
    auto ws = old->workspace();
    if (!ws) return nullptr;
    WORKSPACEID wsID = ws->m_iID;

    // If old is master, return slave; if old is slave, return master
    if (isMaster(old))
        return getPinnedSlave(wsID);
    return getMaster(wsID);
}

std::optional<Vector2D> CTPPAlgorithm::predictSizeForNewTarget() {
    auto parent = m_parent.lock();
    if (!parent) return std::nullopt;
    auto space = parent->space();
    if (!space) return std::nullopt;
    const auto& area = space->workArea();
    // New window will be hidden in queue, but predict slave size
    auto ws = space->workspace();
    if (!ws) return std::nullopt;
    auto& st = stateForWs(ws->m_iID);
    return Vector2D{area.w * (1.0f - st.mfact), area.h};
}

void CTPPAlgorithm::swapTargets(SP<ITarget> a, SP<ITarget> b) {
    if (!a || !b) return;
    auto wsA = a->workspace();
    auto wsB = b->workspace();
    if (!wsA || !wsB || wsA->m_iID != wsB->m_iID) return;

    WORKSPACEID wsID = wsA->m_iID;
    auto& st = stateForWs(wsID);

    // Swap in target list
    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t == a) { wt = b; }
        else if (t == b) { wt = a; }
    }

    // Update pinned slave if involved
    auto pinned = st.pinnedSlave.lock();
    if (pinned == a) st.pinnedSlave = b;
    else if (pinned == b) st.pinnedSlave = a;

    recalculate();
}

void CTPPAlgorithm::moveTargetInDirection(SP<ITarget> t, Math::eDirection dir, bool silent) {
    // For a two-pane layout, only meaningful direction is swapping master/slave
    if (!t) return;
    auto ws = t->workspace();
    if (!ws) return;

    SP<ITarget> master = getMaster(ws->m_iID);
    SP<ITarget> slave  = getPinnedSlave(ws->m_iID);

    if (master && slave)
        swapTargets(master, slave);
}