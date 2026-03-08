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

// Returns all targets for a workspace in insertion order
std::vector<SP<ITarget>> CTPPAlgorithm::targetsForWs(WORKSPACEID wsID) {
    std::vector<SP<ITarget>> result;
    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t && wsIDOf(t) == wsID)
            result.push_back(t);
    }
    return result;
}

SP<ITarget> CTPPAlgorithm::getMaster(WORKSPACEID wsID) {
    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (t && wsIDOf(t) == wsID)
            return t;
    }
    return nullptr;
}

// Get the slave: use remembered slaveWin if valid, else first non-master window
SP<ITarget> CTPPAlgorithm::getEffectiveSlave(WORKSPACEID wsID) {
    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    if (!master) return nullptr;

    // Try remembered slave
    auto remembered = st.slaveWin.lock();
    if (remembered && remembered != master) {
        // Verify it's still in our target list for this workspace
        for (auto& wt : m_targets) {
            if (wt.lock() == remembered)
                return remembered;
        }
        st.slaveWin.reset();
    }

    // Fall back to first non-master window
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

// Hide all windows except master and slave by moving them off-screen
void CTPPAlgorithm::updateVisibility(SP<CSpace> space, WORKSPACEID wsID) {
    auto master = getMaster(wsID);
    auto slave  = getEffectiveSlave(wsID);

    for (auto& wt : m_targets) {
        auto t = wt.lock();
        if (!t || wsIDOf(t) != wsID) continue;
        if (t == master || t == slave) {
            // Make visible — will be positioned by recalculate
            if (t->window() && t->window()->isHidden())
                t->window()->setHidden(false);
        } else {
            // Hide extra windows
            if (t->window() && !t->window()->isHidden())
                t->window()->setHidden(true);
        }
    }
}

// ── Window lifecycle ──────────────────────────────────────────────────────────

void CTPPAlgorithm::newTarget(SP<ITarget> target) {
    if (!target) return;

    for (auto& wt : m_targets)
        if (wt.lock() == target) return;

    WORKSPACEID wsID = wsIDOf(target);
    m_targets.push_back(target);

    // If this is a 3rd+ window, hide it immediately
    auto master = getMaster(wsID);
    auto slave  = getEffectiveSlave(wsID);
    if (target != master && target != slave) {
        if (target->window())
            target->window()->setHidden(true);
    }

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

    // If slave was removed, clear it so getEffectiveSlave picks the next one
    if (st.slaveWin.lock() == target)
        st.slaveWin.reset();

    // If master was removed, promote next window to front
    if (wasMaster) {
        // Find first remaining target for this ws and rotate it to front
        for (auto it = m_targets.begin(); it != m_targets.end(); ++it) {
            auto t = it->lock();
            if (t && wsIDOf(t) == wsID) {
                std::rotate(m_targets.begin(), it, it + 1);
                break;
            }
        }
    }

    // Unhide new effective slave if it was hidden
    auto newSlave = getEffectiveSlave(wsID);
    if (newSlave && newSlave->window() && newSlave->window()->isHidden())
        newSlave->window()->setHidden(false);

    recalculate();
}

// ── Geometry ──────────────────────────────────────────────────────────────────

void CTPPAlgorithm::recalculate() {
    auto space = getSpace();
    if (!space) return;
    auto ws = space->workspace();
    if (!ws) return;

    WORKSPACEID wsID = ws->m_id;
    updateVisibility(space, wsID);

    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    auto  slave  = getEffectiveSlave(wsID);

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

// ── Cycle (mod-tab equivalent) ───────────────────────────────────────────────

void CTPPAlgorithm::cycleNext(WORKSPACEID wsID) {
    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    auto  slave  = getEffectiveSlave(wsID);
    auto  all    = targetsForWs(wsID);

    if (all.size() < 2) return;

    // Find slave's position and pick next non-master window
    SP<ITarget> next;
    bool        found = false;
    for (size_t i = 0; i < all.size(); i++) {
        if (all[i] == slave) { found = true; }
        else if (found && all[i] != master) { next = all[i]; break; }
    }
    // Wrap around
    if (!next) {
        for (auto& t : all) {
            if (t != master && t != slave) { next = t; break; }
        }
    }
    if (!next) return;

    if (slave && slave->window()) slave->window()->setHidden(true);
    if (next->window()) next->window()->setHidden(false);
    st.slaveWin = next;
    recalculate();
}

void CTPPAlgorithm::cyclePrev(WORKSPACEID wsID) {
    auto& st     = stateForWs(wsID);
    auto  master = getMaster(wsID);
    auto  slave  = getEffectiveSlave(wsID);
    auto  all    = targetsForWs(wsID);

    if (all.size() < 2) return;

    SP<ITarget> prev;
    bool        found = false;
    for (int i = (int)all.size() - 1; i >= 0; i--) {
        if (all[i] == slave) { found = true; }
        else if (found && all[i] != master) { prev = all[i]; break; }
    }
    if (!prev) {
        for (int i = (int)all.size() - 1; i >= 0; i--) {
            if (all[i] != master && all[i] != slave) { prev = all[i]; break; }
        }
    }
    if (!prev) return;

    if (slave && slave->window()) slave->window()->setHidden(true);
    if (prev->window()) prev->window()->setHidden(false);
    st.slaveWin = prev;
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

    // Master focused → slaveWin unchanged (persistence!)
    if (isMaster(target)) return;

    // Hidden window somehow focused → ignore
    if (target->window() && target->window()->isHidden()) return;

    // Non-master visible window focused → update slave
    auto& st = stateForWs(wsID);
    if (st.slaveWin.lock() == target) return;

    st.slaveWin = target;
    // No recalculate needed — it's already in the slave position
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
    if (master && slave)
        swapTargets(master, slave);
}