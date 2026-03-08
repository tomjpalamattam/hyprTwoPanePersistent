#include "TwoPanePersistent.hpp"

// ── Helpers ───────────────────────────────────────────────────────────────────

STPPState& CTPPAlgorithm::stateForWs(WORKSPACEID id) {
    return m_wsStates[id];
}

WORKSPACEID CTPPAlgorithm::wsIDOf(SP<ITarget> t) {
    auto ws = t ? t->workspace() : nullptr;
    return ws ? ws->m_id : WORKSPACE_INVALID;
}

SP<CSpace> CTPPAlgorithm::getSpace() {
    auto parent = m_parent.lock();
    return parent ? parent->space() : nullptr;
}

SP<ITarget> CTPPAlgorithm::getMaster(WORKSPACEID wsID) {
    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t && wsIDOf(t) == wsID)
            return t;
    }
    return nullptr;
}

SP<ITarget> CTPPAlgorithm::getPinnedSlave(WORKSPACEID wsID) {
    auto& st = stateForWs(wsID);
    auto  t  = st.pinnedSlave.lock();
    if (!t) return nullptr;
    for (auto& wt : m_targets) {
        if (wt.lock() == t)
            return t;
    }
    st.pinnedSlave.reset();
    return nullptr;
}

bool CTPPAlgorithm::isMaster(SP<ITarget> t) {
    return getMaster(wsIDOf(t)) == t;
}

void CTPPAlgorithm::hideTarget(SP<ITarget> t, SP<CSpace> space) {
    if (t && space)
        t->setSpaceGhost(space);
}

void CTPPAlgorithm::unhideTarget(SP<ITarget> t, SP<CSpace> space) {
    if (t && space)
        t->assignToSpace(space);
}

// ── Window lifecycle ──────────────────────────────────────────────────────────

void CTPPAlgorithm::newTarget(SP<ITarget> target) {
    WORKSPACEID wsID = wsIDOf(target);
    auto& st = stateForWs(wsID);

    SP<ITarget> master = getMaster(wsID);
    SP<ITarget> slave  = getPinnedSlave(wsID);

    m_targets.push_back(target);

    if (!master) {
        // First window → master, no action needed
    } else if (!slave) {
        // Second window → pinned slave
        st.pinnedSlave = target;
    } else {
        // Third+ → hidden queue
        st.hiddenQueue.push_back(target);
        hideTarget(target, getSpace());
        return;
    }

    recalculate();
}

void CTPPAlgorithm::movedTarget(SP<ITarget> target, std::optional<Vector2D>) {
    newTarget(target);
}

void CTPPAlgorithm::removeTarget(SP<ITarget> target) {
    WORKSPACEID wsID = wsIDOf(target);
    auto& st = stateForWs(wsID);

    bool wasMaster = isMaster(target);
    bool wasSlave  = (getPinnedSlave(wsID) == target);

    m_targets.erase(std::remove_if(m_targets.begin(), m_targets.end(),
        [&](const WP<ITarget>& wt) { return wt.lock() == target; }), m_targets.end());

    st.hiddenQueue.erase(std::remove_if(st.hiddenQueue.begin(), st.hiddenQueue.end(),
        [&](const WP<ITarget>& wt) { return wt.lock() == target; }), st.hiddenQueue.end());

    if (wasMaster) {
        // Promote slave to master (move to front of m_targets for this ws)
        auto slaveTarget = getPinnedSlave(wsID);
        if (slaveTarget) {
            auto it = std::find_if(m_targets.begin(), m_targets.end(),
                [&](const WP<ITarget>& wt) { return wt.lock() == slaveTarget; });
            if (it != m_targets.end())
                std::rotate(m_targets.begin(), it, it + 1);
        }
        st.pinnedSlave.reset();
        promoteFromQueue(wsID);
    } else if (wasSlave) {
        st.pinnedSlave.reset();
        promoteFromQueue(wsID);
    }

    recalculate();
}

// ── Geometry ──────────────────────────────────────────────────────────────────

void CTPPAlgorithm::recalculate() {
    auto space = getSpace();
    if (!space) return;
    auto ws = space->workspace();
    if (!ws) return;
    recalculateForSpace(space, ws->m_id);
}

void CTPPAlgorithm::recalculateForSpace(SP<CSpace> space, WORKSPACEID wsID) {
    if (!space) return;

    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    auto  slave  = getPinnedSlave(wsID);

    const CBox& area = space->workArea();

    if (!master) return;

    if (!slave) {
        master->setPositionGlobal(area);
        master->warpPositionSize();
        return;
    }

    double masterW = area.w * (double)st.mfact;
    double slaveW  = area.w - masterW;

    master->setPositionGlobal({area.x, area.y, masterW, area.h});
    master->warpPositionSize();

    slave->setPositionGlobal({area.x + masterW, area.y, slaveW, area.h});
    slave->warpPositionSize();
}

void CTPPAlgorithm::resizeTarget(const Vector2D& delta, SP<ITarget> target, eRectCorner) {
    auto space = getSpace();
    if (!space) return;

    WORKSPACEID wsID  = wsIDOf(target);
    auto&       st    = stateForWs(wsID);
    double      totalW = space->workArea().w;
    if (totalW <= 0) return;

    if (isMaster(target))
        st.mfact += (float)(delta.x / totalW);
    else
        st.mfact -= (float)(delta.x / totalW);

    st.mfact = std::clamp(st.mfact, 0.1f, 0.9f);
    recalculate();
}

// ── Cycle ─────────────────────────────────────────────────────────────────────

void CTPPAlgorithm::promoteFromQueue(WORKSPACEID wsID) {
    auto& st    = stateForWs(wsID);
    auto  space = getSpace();
    while (!st.hiddenQueue.empty()) {
        auto next = st.hiddenQueue.front().lock();
        st.hiddenQueue.pop_front();
        if (next) {
            unhideTarget(next, space);
            st.pinnedSlave = next;
            return;
        }
    }
}

void CTPPAlgorithm::cycleNext(WORKSPACEID wsID) {
    auto& st    = stateForWs(wsID);
    auto  space = getSpace();
    if (st.hiddenQueue.empty()) return;

    auto currentSlave = getPinnedSlave(wsID);

    SP<ITarget> next;
    while (!st.hiddenQueue.empty()) {
        next = st.hiddenQueue.front().lock();
        st.hiddenQueue.pop_front();
        if (next) break;
        next = nullptr;
    }
    if (!next) return;

    if (currentSlave) {
        hideTarget(currentSlave, space);
        st.hiddenQueue.push_back(currentSlave);
    }

    unhideTarget(next, space);
    st.pinnedSlave = next;
    recalculate();
}

void CTPPAlgorithm::cyclePrev(WORKSPACEID wsID) {
    auto& st    = stateForWs(wsID);
    auto  space = getSpace();
    if (st.hiddenQueue.empty()) return;

    auto currentSlave = getPinnedSlave(wsID);

    SP<ITarget> prev;
    while (!st.hiddenQueue.empty()) {
        prev = st.hiddenQueue.back().lock();
        st.hiddenQueue.pop_back();
        if (prev) break;
        prev = nullptr;
    }
    if (!prev) return;

    if (currentSlave) {
        hideTarget(currentSlave, space);
        st.hiddenQueue.push_front(currentSlave);
    }

    unhideTarget(prev, space);
    st.pinnedSlave = prev;
    recalculate();
}

// ── layoutMsg ────────────────────────────────────────────────────────────────

std::expected<void, std::string> CTPPAlgorithm::layoutMsg(const std::string_view& sv) {
    auto space = getSpace();
    if (!space) return {};
    auto ws = space->workspace();
    if (!ws) return {};
    WORKSPACEID wsID = ws->m_id;

    if (sv == "cyclenext")
        cycleNext(wsID);
    else if (sv == "cycleprev")
        cyclePrev(wsID);
    else if (sv == "focusmaster") {
        auto master = getMaster(wsID);
        if (master)
            space->getNextCandidate(master); // move focus via space
    } else if (sv == "focusslave") {
        auto slave = getPinnedSlave(wsID);
        if (slave)
            space->getNextCandidate(slave);
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

// ── Focus hook target ─────────────────────────────────────────────────────────

void CTPPAlgorithm::onTargetFocused(SP<ITarget> target) {
    if (!target) return;
    WORKSPACEID wsID = wsIDOf(target);
    if (wsID == WORKSPACE_INVALID) return;

    if (isMaster(target))
        return; // master focused — slave stays pinned (the persistence)

    // Verify it's one of our visible (non-hidden) targets
    bool found = false;
    for (auto& wt : m_targets) {
        if (wt.lock() == target) { found = true; break; }
    }
    if (!found) return;

    auto& st = stateForWs(wsID);
    if (st.pinnedSlave.lock() == target) return;

    st.pinnedSlave = target;
    recalculate();
}

// ── Remaining stubs ───────────────────────────────────────────────────────────

SP<ITarget> CTPPAlgorithm::getNextCandidate(SP<ITarget> old) {
    if (!old) return nullptr;
    WORKSPACEID wsID = wsIDOf(old);
    if (isMaster(old))
        return getPinnedSlave(wsID);
    return getMaster(wsID);
}

std::optional<Vector2D> CTPPAlgorithm::predictSizeForNewTarget() {
    auto space = getSpace();
    if (!space) return std::nullopt;
    auto ws = space->workspace();
    if (!ws) return std::nullopt;
    const auto& area = space->workArea();
    float mfact = stateForWs(ws->m_id).mfact;
    return Vector2D{area.w * (double)(1.0f - mfact), area.h};
}

void CTPPAlgorithm::swapTargets(SP<ITarget> a, SP<ITarget> b) {
    if (!a || !b) return;
    WORKSPACEID wsA = wsIDOf(a);
    WORKSPACEID wsB = wsIDOf(b);
    if (wsA != wsB) return;

    auto& st = stateForWs(wsA);

    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t == a)      wt = b;
        else if (t == b) wt = a;
    }

    auto pinned = st.pinnedSlave.lock();
    if (pinned == a)      st.pinnedSlave = b;
    else if (pinned == b) st.pinnedSlave = a;

    recalculate();
}

void CTPPAlgorithm::moveTargetInDirection(SP<ITarget> t, Math::eDirection, bool) {
    if (!t) return;
    WORKSPACEID wsID = wsIDOf(t);
    auto master = getMaster(wsID);
    auto slave  = getPinnedSlave(wsID);
    if (master && slave)
        swapTargets(master, slave);
}