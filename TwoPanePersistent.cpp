#include "TwoPanePersistent.hpp"

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#undef private

#include <hyprland/src/helpers/Monitor.hpp>

// ── Helpers ───────────────────────────────────────────────────────────────────

STPPNodeData* CTwoPanePersistentLayout::getNodeFromWindow(PHLWINDOW pWindow) {
    for (auto& node : m_lNodes) {
        if (node.pWindow.lock() == pWindow)
            return &node;
    }
    return nullptr;
}

STPPWorkspaceData& CTwoPanePersistentLayout::getOrCreateWorkspaceData(int wsID) {
    auto it = m_mWorkspaceData.find(wsID);
    if (it == m_mWorkspaceData.end()) {
        m_mWorkspaceData[wsID] = STPPWorkspaceData{};
        m_mWorkspaceData[wsID].workspaceID = wsID;
    }
    return m_mWorkspaceData[wsID];
}

STPPNodeData* CTwoPanePersistentLayout::getMasterNode(int wsID) {
    for (auto& node : m_lNodes) {
        if (node.workspaceID == wsID && node.isMaster && node.valid)
            return &node;
    }
    return nullptr;
}

STPPNodeData* CTwoPanePersistentLayout::getSlaveNode(int wsID) {
    PHLWINDOW pinned = getPinnedSlave(wsID);
    if (!pinned)
        return nullptr;
    return getNodeFromWindow(pinned);
}

PHLWINDOW CTwoPanePersistentLayout::getPinnedSlave(int wsID) {
    auto& ws = getOrCreateWorkspaceData(wsID);
    auto  pWindow = ws.pinnedSlave.lock();

    if (!pWindow || !pWindow->m_bIsMapped || pWindow->isHidden())
        return nullptr;

    // Verify it's still in our node list as a non-master on this workspace
    auto* node = getNodeFromWindow(pWindow);
    if (!node || node->workspaceID != wsID || node->isMaster)
        return nullptr;

    return pWindow;
}

void CTwoPanePersistentLayout::promoteFromQueue(int wsID) {
    auto& ws = getOrCreateWorkspaceData(wsID);

    while (!ws.hiddenQueue.empty()) {
        auto next = ws.hiddenQueue.front().lock();
        ws.hiddenQueue.pop_front();

        if (next && next->m_bIsMapped) {
            unhideWindow(next, wsID);
            ws.pinnedSlave = next;
            return;
        }
    }
    // Queue exhausted — no slave
    ws.pinnedSlave.reset();
}

void CTwoPanePersistentLayout::hideWindow(PHLWINDOW pWindow) {
    // Move to special:tpp_hidden workspace silently
    g_pCompositor->moveWindowToWorkspaceSafe(pWindow,
        g_pCompositor->getWorkspaceByName("special:tpp_hidden", true));
}

void CTwoPanePersistentLayout::unhideWindow(PHLWINDOW pWindow, int targetWsID) {
    auto pWs = g_pCompositor->getWorkspaceByID(targetWsID);
    if (pWs)
        g_pCompositor->moveWindowToWorkspaceSafe(pWindow, pWs);
}

// ── Focus hook ────────────────────────────────────────────────────────────────
// Called whenever focus changes. This is the core of TwoPanePersistent:
//   - If a SLAVE is focused → pin it (update slaveWin)
//   - If MASTER is focused → do nothing (slave stays pinned)

void CTwoPanePersistentLayout::onWindowFocused(PHLWINDOW pWindow) {
    if (!pWindow || !pWindow->m_bIsMapped)
        return;

    auto* node = getNodeFromWindow(pWindow);
    if (!node || node->isMaster)
        return; // master focused — persistence: don't touch pinnedSlave

    int wsID = node->workspaceID;
    auto& ws = getOrCreateWorkspaceData(wsID);

    PHLWINDOW currentPinned = getPinnedSlave(wsID);

    // If a different slave is now focused, update the pin
    if (pWindow != currentPinned) {
        // The previously pinned slave (if any) was already on screen —
        // it stays on screen. The newly focused window becomes the pin.
        ws.pinnedSlave = pWindow;
        // recalculate so geometry is correct (master stays left, slave right)
        recalculateMonitor(pWindow->m_pMonitor.lock()->ID);
    }
}

// ── IHyprLayout: window lifecycle ─────────────────────────────────────────────

void CTwoPanePersistentLayout::onWindowCreatedTiling(PHLWINDOW pWindow, eDirection) {
    if (!pWindow->m_pWorkspace)
        return;

    int wsID = pWindow->m_pWorkspace->m_iID;
    auto& ws = getOrCreateWorkspaceData(wsID);

    STPPNodeData node;
    node.pWindow     = pWindow;
    node.workspaceID = wsID;
    node.valid       = true;

    bool hasMaster = getMasterNode(wsID) != nullptr;
    bool hasSlave  = getPinnedSlave(wsID) != nullptr;

    if (!hasMaster) {
        // First window → becomes master
        node.isMaster = true;
        m_lNodes.push_back(node);
    } else if (!hasSlave) {
        // Second window → becomes the pinned slave
        node.isMaster = false;
        m_lNodes.push_back(node);
        ws.pinnedSlave = pWindow;
    } else {
        // Third+ window → goes into the hidden queue
        node.isMaster = false;
        m_lNodes.push_back(node);
        ws.hiddenQueue.push_back(pWindow);
        hideWindow(pWindow);
        return; // no recalculate needed, window is hidden
    }

    recalculateMonitor(pWindow->m_pMonitor.lock()->ID);
}

void CTwoPanePersistentLayout::onWindowRemovedTiling(PHLWINDOW pWindow) {
    auto* node = getNodeFromWindow(pWindow);
    if (!node)
        return;

    int  wsID      = node->workspaceID;
    bool wasMaster = node->isMaster;

    node->valid = false;
    m_lNodes.remove_if([](const STPPNodeData& n) { return !n.valid; });

    auto& ws = getOrCreateWorkspaceData(wsID);

    if (wasMaster) {
        // Master closed: promote the current slave to master
        auto* slaveNode = getSlaveNode(wsID);
        if (slaveNode) {
            slaveNode->isMaster = true;
            ws.pinnedSlave.reset();
            // Then promote from queue to fill slave slot
            promoteFromQueue(wsID);
        }
    } else {
        // Slave closed: promote from queue
        if (ws.pinnedSlave.lock() == pWindow || !getPinnedSlave(wsID)) {
            ws.pinnedSlave.reset();
            promoteFromQueue(wsID);
        } else {
            // One of the hidden queue windows closed — clean it from the queue
            ws.hiddenQueue.erase(
                std::remove_if(ws.hiddenQueue.begin(), ws.hiddenQueue.end(),
                    [&](const PHLWINDOWREF& ref) { return ref.lock() == pWindow; }),
                ws.hiddenQueue.end());
        }
    }

    auto pWs = g_pCompositor->getWorkspaceByID(wsID);
    if (pWs && pWs->m_pMonitor.lock())
        recalculateMonitor(pWs->m_pMonitor.lock()->ID);
}

bool CTwoPanePersistentLayout::isWindowTiled(PHLWINDOW pWindow) {
    return getNodeFromWindow(pWindow) != nullptr;
}

// ── Geometry ──────────────────────────────────────────────────────────────────

void CTwoPanePersistentLayout::recalculateWorkspace(PHLWORKSPACE pWorkspace) {
    if (!pWorkspace)
        return;

    int wsID = pWorkspace->m_iID;
    auto& ws = getOrCreateWorkspaceData(wsID);

    auto* masterNode = getMasterNode(wsID);
    auto* slaveNode  = getSlaveNode(wsID);

    auto pMonitor = pWorkspace->m_pMonitor.lock();
    if (!pMonitor)
        return;

    // Usable area (respects gaps, reserved areas)
    CBox workArea = pMonitor->vecPosition;
    workArea.x += pMonitor->vecReservedTopLeft.x;
    workArea.y += pMonitor->vecReservedTopLeft.y;
    workArea.w  = pMonitor->vecSize.x - pMonitor->vecReservedTopLeft.x - pMonitor->vecReservedBottomRight.x;
    workArea.h  = pMonitor->vecSize.y - pMonitor->vecReservedTopLeft.y - pMonitor->vecReservedBottomRight.y;

    if (!masterNode) {
        // No windows at all
        return;
    }

    if (!slaveNode) {
        // Only master — takes full area
        masterNode->position = {workArea.x, workArea.y};
        masterNode->size     = {workArea.w, workArea.h};
        applyNodeDataToWindow(masterNode);
        return;
    }

    // Two pane: master left, slave right
    float masterW = workArea.w * ws.mfact;
    float slaveW  = workArea.w - masterW;

    masterNode->position = {workArea.x, workArea.y};
    masterNode->size     = {masterW, workArea.h};

    slaveNode->position = {workArea.x + masterW, workArea.y};
    slaveNode->size     = {slaveW, workArea.h};

    applyNodeDataToWindow(masterNode);
    applyNodeDataToWindow(slaveNode);
}

void CTwoPanePersistentLayout::recalculateMonitor(const MONITORID& monID) {
    auto pMonitor = g_pCompositor->getMonitorFromID(monID);
    if (!pMonitor)
        return;

    for (auto& pWorkspace : g_pCompositor->m_vWorkspaces) {
        if (pWorkspace->m_pMonitor.lock() == pMonitor)
            recalculateWorkspace(pWorkspace);
    }
}

void CTwoPanePersistentLayout::recalculateWindow(PHLWINDOW pWindow) {
    auto* node = getNodeFromWindow(pWindow);
    if (!node)
        return;
    auto pWs = g_pCompositor->getWorkspaceByID(node->workspaceID);
    if (pWs)
        recalculateWorkspace(pWs);
}

void CTwoPanePersistentLayout::applyNodeDataToWindow(STPPNodeData* node) {
    auto pWindow = node->pWindow.lock();
    if (!pWindow)
        return;

    pWindow->m_vPosition = node->position;
    pWindow->m_vSize     = node->size;
    pWindow->updateWindowDecos();

    const auto PMONITOR = g_pCompositor->getMonitorFromID(pWindow->m_iMonitorID);
    if (PMONITOR)
        pWindow->m_bIsFloating = false;
}

// ── Resize ────────────────────────────────────────────────────────────────────

void CTwoPanePersistentLayout::resizeActiveWindow(const Vector2D& delta, eRectCorner corner, PHLWINDOW pWindow) {
    auto pFocused = pWindow ? pWindow : g_pCompositor->m_pLastWindow.lock();
    if (!pFocused)
        return;

    auto* node = getNodeFromWindow(pFocused);
    if (!node)
        return;

    auto& ws = getOrCreateWorkspaceData(node->workspaceID);

    // Adjust mfact based on horizontal drag
    auto pWs = g_pCompositor->getWorkspaceByID(node->workspaceID);
    if (!pWs)
        return;
    auto pMon = pWs->m_pMonitor.lock();
    if (!pMon)
        return;

    float totalW = pMon->vecSize.x - pMon->vecReservedTopLeft.x - pMon->vecReservedBottomRight.x;
    if (totalW <= 0)
        return;

    if (node->isMaster)
        ws.mfact += delta.x / totalW;
    else
        ws.mfact -= delta.x / totalW;

    ws.mfact = std::clamp(ws.mfact, 0.1f, 0.9f);
    recalculateWorkspace(pWs);
}

// ── Cycle dispatcher ──────────────────────────────────────────────────────────

void CTwoPanePersistentLayout::cycleNext(PHLWORKSPACE pWorkspace) {
    if (!pWorkspace)
        return;

    int wsID = pWorkspace->m_iID;
    auto& ws = getOrCreateWorkspaceData(wsID);

    if (ws.hiddenQueue.empty())
        return;

    PHLWINDOW currentSlave = getPinnedSlave(wsID);

    // Pop next from front of queue
    PHLWINDOW next;
    while (!ws.hiddenQueue.empty()) {
        next = ws.hiddenQueue.front().lock();
        ws.hiddenQueue.pop_front();
        if (next && next->m_bIsMapped)
            break;
        next = nullptr;
    }

    if (!next)
        return;

    // Send current slave to back of queue and hide it
    if (currentSlave) {
        ws.hiddenQueue.push_back(currentSlave);
        hideWindow(currentSlave);

        // Remove its node from visible tracking (still in m_lNodes, just hidden)
    }

    // Bring next window in and pin it
    unhideWindow(next, wsID);
    ws.pinnedSlave = next;

    recalculateMonitor(pWorkspace->m_pMonitor.lock()->ID);
    g_pCompositor->focusWindow(next);
}

void CTwoPanePersistentLayout::cyclePrev(PHLWORKSPACE pWorkspace) {
    if (!pWorkspace)
        return;

    int wsID = pWorkspace->m_iID;
    auto& ws = getOrCreateWorkspaceData(wsID);

    if (ws.hiddenQueue.empty())
        return;

    PHLWINDOW currentSlave = getPinnedSlave(wsID);

    // Take from back of queue
    PHLWINDOW prev;
    while (!ws.hiddenQueue.empty()) {
        prev = ws.hiddenQueue.back().lock();
        ws.hiddenQueue.pop_back();
        if (prev && prev->m_bIsMapped)
            break;
        prev = nullptr;
    }

    if (!prev)
        return;

    if (currentSlave) {
        ws.hiddenQueue.push_front(currentSlave);
        hideWindow(currentSlave);
    }

    unhideWindow(prev, wsID);
    ws.pinnedSlave = prev;

    recalculateMonitor(pWorkspace->m_pMonitor.lock()->ID);
    g_pCompositor->focusWindow(prev);
}

// ── layoutMessage (dispatcher target) ────────────────────────────────────────

std::any CTwoPanePersistentLayout::layoutMessage(SLayoutMessageHeader header, std::string message) {
    if (message == "cyclenext") {
        cycleNext(header.pWindow ? header.pWindow->m_pWorkspace : g_pCompositor->m_pLastWindow.lock()->m_pWorkspace);
    } else if (message == "cycleprev") {
        cyclePrev(header.pWindow ? header.pWindow->m_pWorkspace : g_pCompositor->m_pLastWindow.lock()->m_pWorkspace);
    } else if (message == "focusmaster") {
        auto* masterNode = header.pWindow ? getMasterNode(header.pWindow->m_pWorkspace->m_iID) : nullptr;
        if (masterNode)
            g_pCompositor->focusWindow(masterNode->pWindow.lock());
    } else if (message == "focusslave") {
        if (header.pWindow) {
            PHLWINDOW slave = getPinnedSlave(header.pWindow->m_pWorkspace->m_iID);
            if (slave)
                g_pCompositor->focusWindow(slave);
        }
    }
    return "";
}

// ── Stubs for interface completeness ─────────────────────────────────────────

SWindowRenderLayoutHints CTwoPanePersistentLayout::requestRenderHints(PHLWINDOW) {
    return {};
}

void CTwoPanePersistentLayout::switchWindows(PHLWINDOW pA, PHLWINDOW pB) {
    auto* nodeA = getNodeFromWindow(pA);
    auto* nodeB = getNodeFromWindow(pB);
    if (!nodeA || !nodeB)
        return;

    std::swap(nodeA->isMaster, nodeB->isMaster);

    if (nodeA->workspaceID == nodeB->workspaceID) {
        auto& ws = getOrCreateWorkspaceData(nodeA->workspaceID);
        if (ws.pinnedSlave.lock() == pA)
            ws.pinnedSlave = pB;
        else if (ws.pinnedSlave.lock() == pB)
            ws.pinnedSlave = pA;
    }

    recalculateWindow(pA);
}

void CTwoPanePersistentLayout::moveWindowTo(PHLWINDOW pWindow, const std::string& dir, bool silent) {
    // Not implemented for this layout — would need workspace transfers
}

void CTwoPanePersistentLayout::alterSplitRatio(PHLWINDOW pWindow, float delta, bool exact) {
    auto* node = getNodeFromWindow(pWindow);
    if (!node)
        return;
    auto& ws = getOrCreateWorkspaceData(node->workspaceID);
    if (exact)
        ws.mfact = delta;
    else
        ws.mfact = std::clamp(ws.mfact + delta, 0.1f, 0.9f);
    recalculateWindow(pWindow);
}

std::string CTwoPanePersistentLayout::getLayoutName() {
    return "TwoPanePersistent";
}

void CTwoPanePersistentLayout::replaceWindowDataWith(PHLWINDOW from, PHLWINDOW to) {
    auto* node = getNodeFromWindow(from);
    if (!node)
        return;
    node->pWindow = to;

    for (auto& [wsID, ws] : m_mWorkspaceData) {
        if (ws.pinnedSlave.lock() == from)
            ws.pinnedSlave = to;
        for (auto& ref : ws.hiddenQueue) {
            if (ref.lock() == from)
                ref = to;
        }
    }
}

Vector2D CTwoPanePersistentLayout::predictSizeForNewWindowTiled() {
    return {};
}

void CTwoPanePersistentLayout::fullscreenRequestForWindow(PHLWINDOW pWindow,
    const eFullscreenMode CURRENT_EFFECTIVE_MODE, const eFullscreenMode EFFECTIVE_MODE) {
    // Delegate to compositor default fullscreen handling
}
