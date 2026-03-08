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
    auto parent = IModeAlgorithm::m_parent.lock();
    return parent ? parent->space() : nullptr;
}

std::vector<SP<ITarget>> CTPPAlgorithm::targetsForWs(WORKSPACEID wsID) {
    std::vector<SP<ITarget>> result;
    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t && wsIDOf(t) == wsID)
            result.push_back(t);
    }
    return result;
}

// Master = first target in insertion order for this workspace
SP<ITarget> CTPPAlgorithm::getMaster(WORKSPACEID wsID) {
    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t && wsIDOf(t) == wsID)
            return t;
    }
    return nullptr;
}

// Slave = remembered slaveWin if valid, else first non-master
SP<ITarget> CTPPAlgorithm::getEffectiveSlave(WORKSPACEID wsID) {
    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    if (!master) return nullptr;

    auto remembered = st.slaveWin.lock();
    if (remembered && remembered != master) {
        for (auto& wt : m_targets)
            if (wt.lock() == remembered)
                return remembered;
        st.slaveWin.reset();
    }

    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t && wsIDOf(t) == wsID && t != master)
            return t;
    }
    return nullptr;
}

bool CTPPAlgorithm::isMaster(SP<ITarget> t) {
    if (!t) return false;
    return getMaster(wsIDOf(t)) == t;
}

// ── Window lifecycle ──────────────────────────────────────────────────────────

void CTPPAlgorithm::newTarget(SP<ITarget> target) {
    if (!target) return;
    for (auto& wt : m_targets)
        if (wt.lock() == target) return;
    m_targets.push_back(target);
    recalculate();
}

void CTPPAlgorithm::movedTarget(SP<ITarget> target, std::optional<Vector2D>) {
    if (!target) return;
    for (auto& wt : m_targets)
        if (wt.lock() == target) { recalculate(); return; }
    newTarget(target);
}

void CTPPAlgorithm::removeTarget(SP<ITarget> target) {
    if (!target) return;

    WORKSPACEID wsID = wsIDOf(target);
    auto& st = stateForWs(wsID);

    bool wasMaster = isMaster(target);

    m_targets.erase(std::remove_if(m_targets.begin(), m_targets.end(),
        [&](const WP<ITarget>& wt) { return wt.lock() == target; }), m_targets.end());

    if (st.slaveWin.lock() == target)
        st.slaveWin.reset();

    if (wasMaster) {
        for (auto it = m_targets.begin(); it != m_targets.end(); ++it) {
            auto t = it->lock();
            if (t && wsIDOf(t) == wsID) {
                std::rotate(m_targets.begin(), it, it + 1);
                break;
            }
        }
    }

    recalculate();
}

// ── Geometry ──────────────────────────────────────────────────────────────────

void CTPPAlgorithm::recalculate() {
    auto space = getSpace();
    if (!space) return;
    auto ws = space->workspace();
    if (!ws) return;

    WORKSPACEID wsID = ws->m_id;
    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    auto  slave  = getEffectiveSlave(wsID);
    auto  all    = targetsForWs(wsID);

    const CBox& area = space->workArea();
    if (!master) return;

    // Park all extra windows offscreen (outside right edge)
    double offX = area.x + area.w + 10;
    double offY = area.y;
    for (auto& t : all) {
        if (t == master || t == slave) continue;
        t->setPositionGlobal({offX, offY, 1.0, 1.0});
        t->warpPositionSize();
    }

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

    WORKSPACEID wsID   = wsIDOf(target);
    auto&       st     = stateForWs(wsID);
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

void CTPPAlgorithm::cycleNext(WORKSPACEID wsID) {
    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    auto  slave  = getEffectiveSlave(wsID);
    auto  all    = targetsForWs(wsID);

    if (all.size() < 3) return;

    int slaveIdx = -1;
    for (int i = 0; i < (int)all.size(); i++)
        if (all[i] == slave) { slaveIdx = i; break; }

    for (int i = 1; i <= (int)all.size(); i++) {
        int idx = (slaveIdx + i) % (int)all.size();
        if (all[idx] != master) {
            st.slaveWin = all[idx];
            recalculate();
            return;
        }
    }
}

void CTPPAlgorithm::cyclePrev(WORKSPACEID wsID) {
    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    auto  slave  = getEffectiveSlave(wsID);
    auto  all    = targetsForWs(wsID);

    if (all.size() < 3) return;

    int slaveIdx = -1;
    for (int i = 0; i < (int)all.size(); i++)
        if (all[i] == slave) { slaveIdx = i; break; }

    for (int i = 1; i <= (int)all.size(); i++) {
        int idx = ((slaveIdx - i) % (int)all.size() + (int)all.size()) % (int)all.size();
        if (all[idx] != master) {
            st.slaveWin = all[idx];
            recalculate();
            return;
        }
    }
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
        if (master) space->getNextCandidate(master);
    } else if (sv == "focusslave") {
        auto slave = getEffectiveSlave(wsID);
        if (slave) space->getNextCandidate(slave);
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

// ── Focus tracking (the "persistent" part) ───────────────────────────────────

void CTPPAlgorithm::onTargetFocused(SP<ITarget> target) {
    if (!target) return;
    WORKSPACEID wsID = wsIDOf(target);
    if (wsID == WORKSPACE_INVALID) return;

    // Master focused → slave stays unchanged (persistence!)
    if (isMaster(target)) return;

    auto& st    = stateForWs(wsID);
    auto  slave = getEffectiveSlave(wsID);

    // Only update slave record if this is the visible slave
    if (target == slave)
        st.slaveWin = target;
    else {
        // Offscreen window got focus → redirect to slave
        auto space = getSpace();
        if (space && slave) space->getNextCandidate(slave);
    }
}

// ── Navigation ────────────────────────────────────────────────────────────────

SP<ITarget> CTPPAlgorithm::getNextCandidate(SP<ITarget> old) {
    if (!old) return nullptr;
    WORKSPACEID wsID = wsIDOf(old);
    if (isMaster(old)) return getEffectiveSlave(wsID);
    return getMaster(wsID);
}

std::optional<Vector2D> CTPPAlgorithm::predictSizeForNewTarget() {
    auto space = getSpace();
    if (!space) return std::nullopt;
    auto ws = space->workspace();
    if (!ws) return std::nullopt;
    const auto& area  = space->workArea();
    float       mfact = stateForWs(ws->m_id).mfact;
    return Vector2D{area.w * (double)(1.0f - mfact), area.h};
}

void CTPPAlgorithm::swapTargets(SP<ITarget> a, SP<ITarget> b) {
    if (!a || !b) return;
    WORKSPACEID wsID = wsIDOf(a);
    if (wsID != wsIDOf(b)) return;

    auto& st = stateForWs(wsID);
    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t == a)      wt = b;
        else if (t == b) wt = a;
    }
    auto pinned = st.slaveWin.lock();
    if (pinned == a)      st.slaveWin = b;
    else if (pinned == b) st.slaveWin = a;
    recalculate();
}

void CTPPAlgorithm::moveTargetInDirection(SP<ITarget> t, Math::eDirection, bool) {
    if (!t) return;
    WORKSPACEID wsID = wsIDOf(t);
    auto master = getMaster(wsID);
    auto slave  = getEffectiveSlave(wsID);
    if (master && slave) swapTargets(master, slave);
}