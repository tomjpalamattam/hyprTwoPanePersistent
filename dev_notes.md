# Development notes

Everything learned while porting XMonad's `TwoPanePersistent` to a Hyprland
plugin. Written down because most of it is not in the wiki, and all of it cost
a debugging cycle to find.

Target: Hyprland **0.56.2** (`efb5099`), also builds against `main`.

---

## 1. The layout API was rewritten in 0.55

`IHyprLayout` and `HyprlandAPI::addLayout` are both `[[deprecated]]`. Custom
layouts now implement `Layout::ITiledAlgorithm` and register with:

```cpp
HyprlandAPI::addTiledAlgo(handle, "twopanepersistent", &typeid(CTwoPanePersistent),
                          []() -> UP<Layout::ITiledAlgorithm> {
                              return makeUnique<CTwoPanePersistent>();
                          });
```

Methods to implement: `newTarget`, `movedTarget`, `removeTarget`, `resizeTarget`,
`recalculate`, `swapTargets`, `moveTargetInDirection`, `getNextCandidate`,
`layoutMsg`, `predictSizeForNewTarget`, `layoutName`.

Key concepts:

| Thing                        | What it is                                                                                |
| ---------------------------- | ----------------------------------------------------------------------------------------- |
| `ITarget`                    | One tiled item. **Not** one window — see §4.                                              |
| `CSpace`                     | Per-workspace container. `space()->workArea()` is your canvas.                            |
| `CAlgorithm`                 | Owns the tiled + floating algorithms for a space. Reachable via the protected `m_parent`. |
| `m_defaultFullscreenHandler` | Protected member; call `syncTargetSizeAndPosition()` and bail early when fullscreen.      |

**One algorithm instance exists per workspace.** All state (`m_nodes`, `m_slave`,
`m_split`) is therefore per-workspace for free. Don't add workspace IDs to it.

A Lua layout (`hl.layout.register`) cannot do this job: the Lua context only
exposes `target:place(box)`. No hiding, no focus control. C++ is required.

### Best reference implementations

Read these before writing anything — they are the real documentation:

- `src/layout/algorithm/tiled/monocle/MonocleAlgorithm.cpp` — hiding windows,
  focus-event subscription, cycling. Closest analogue to this layout.
- `src/config/lua/layout/LuaLayoutProvider.cpp` — minimal complete
  `ITiledAlgorithm` outside the built-ins.
- `src/layout/algorithm/tiled/master/MasterAlgorithm.cpp` — mfact/orientation.

---

## 2. Version drift is the #1 source of breakage

Hyprland guarantees **no** plugin API stability. The wiki says internal methods
may change "without any prior notice", and it means it.

Between 0.56.2 and `main`, `CWindow` was decomposed:

|                  | 0.56.2                    | `main`                                              |
| ---------------- | ------------------------- | --------------------------------------------------- |
| Header           | `desktop/view/Window.hpp` | `desktop/view/window/Window.hpp`                    |
| Alpha / anim     | directly on `CWindow`     | behind `.presentation()` (`WindowPresentation.hpp`) |
| Input-block enum | `INPUT_BLOCK_*`           | `FOCUS_BLOCK_*` (moved to `view/focusable/`)        |

`main` also adds ~15 new files under `desktop/view/window/` (`WindowBackend`,
`WindowMetadata`, `WindowEffectsController`, …) and a `view/focusable/` mixin.

**What did NOT change** (verified by diffing the trees): `ITiledAlgorithm`,
`IModeAlgorithm`, `CAlgorithm`, `CSpace`, `ITarget`, `FocusState`, `EventBus`,
`MonitorState`, `ConfigValue`, `FloatValue`. The layout interfaces are stable;
the window/view layer is what moves.

### How this repo stays portable

One `__has_include` probe, set once at the top of `TwoPanePersistent.cpp`:

```cpp
#if __has_include(<hyprland/src/desktop/view/window/Window.hpp>)
#define TPP_HYPRLAND_SPLIT_WINDOW 1
...
```

Only **two** version-conditional spots remain: the include block and
`setAnimationsToMove()`. Keep it that way — prefer APIs that are identical on
both (see §5) over adding more `#if`.

### Debugging drift

Don't guess at header paths. Download the exact tag and check:

```sh
curl -sL -o hl.tar.gz \
  "https://codeload.github.com/hyprwm/Hyprland/tar.gz/refs/tags/v0.56.2"
tar tzf hl.tar.gz | grep "desktop/view"
```

Then `diff` the specific header against `main` to see what actually moved.

---

## 3. Grouping does NOT go through `newTarget` ← the big one

This caused windows to vanish and was invisible from the outside.

When you group windows, `CGroup::init()` does **not** remove the old targets and
add a new one. It calls:

```
CGroup::init()
  → LayoutManager::switchTargets(windowTarget, groupTarget)
    → ITarget::swap()
      → CSpace::swap()
        → CAlgorithm::swapTargets()
          → YOUR swapTargets(a, b)
```

The group target **replaces** a window target in place. So `swapTargets` is
called with one target that is in your node list and one that has never been
seen before.

The naive implementation is wrong:

```cpp
// WRONG — silently does nothing when a group forms
if (IA < 0 || IB < 0)
    return;
```

`indexOf(newGroupTarget)` is `-1`, so this bails, the stale window target stays
in `m_nodes`, and the layout keeps positioning and hiding a target Hyprland has
already swapped out. It never gets `removeTarget`, so it is never un-hidden →
the window disappears permanently.

**A one-sided match is a replacement, not a no-op.** Hyprland's own
`CAlgorithm::swapTargets` does exactly this (`*ia = b`). Mirror it, and un-hide
the outgoing target before dropping it.

Related: group members get `setSpaceGhost()` — they belong to a space but are
deliberately not sent to the layout. Only the group target is.

---

## 4. `ITarget` is not a window

`ITarget::window()` on a **group** returns only `m_group->current()` — the
active tab. Anything that must affect the whole group (hiding!) has to go
through `CWindowGroupTarget::getGroup()->windows()`.

```cpp
if (t->type() == TARGET_TYPE_GROUP) {
    const auto GT = dynamicPointerCast<CWindowGroupTarget>(t);   // not reinterpret
    ...GT->getGroup()->windows()...
}
```

Use `dynamicPointerCast` — `ITarget` is polymorphic, and it returns null on a
type mismatch instead of producing garbage.

Conversely `CWindow::layoutTarget()` returns `m_group ? m_group->m_target :
m_target`, so a focus event on _any_ tab correctly resolves to the group's node.
That direction needs no special handling.

---

## 5. Hiding windows: alpha vs `setHidden`

**Do not use `WINDOW_ALPHA_LAYOUT`.** `CGroup::updateWindowVisibility()` rewrites
it on every tab switch (active → `1.F`, inactive → `0.F`). A layout that also
writes it loses the race, and a hidden grouped window pops back into view when
you press Tab.

There are two viable mechanisms. **We use alpha.** The trade-off is real and was
settled by wanting animation, not by one being strictly better.

### `setHidden(bool)` — the simple one

- Identical signature and semantics on both versions → no `#if` needed.
- Gates rendering _and_ input (`acceptsInput()` is `!isHidden() && !isInputBlocked()`),
  so no separate `setInputBlocked` call, which avoids the `INPUT_BLOCK_*` /
  `FOCUS_BLOCK_*` naming split entirely.
- Calls `setSuspended(true)`: the client stops producing frames. Background
  windows cost nothing.
- **Has a safety net.** `CAlgorithm::updateTiledAlgo` force-calls
  `setHidden(false)` on every layout switch, commented as a safeguard "for
  layouts (including third-party plugins) that use setHidden".
- Semantically closest to XMonad, which genuinely unmaps non-visible windows.
- **No animation.** It is a binary map/unmap, so a revealed window pops in.

### Alpha — what this plugin actually does

Alpha channels are `PHLANIMVAR<float>`, so assigning one plays Hyprland's real
fade and respects the user's animation config. That is the entire reason to
prefer it. Everything else about it is worse:

- **No safety net.** `updateTiledAlgo` resets `setHidden` only — nothing in
  Hyprland ever resets a custom alpha channel. `PLUGIN_EXIT` is not called when
  a plugin faults, so a crash while windows are hidden strands them invisible
  until the compositor restarts. Correct un-hiding is load-bearing, not tidy.
- **Costs resources.** Alpha-0 windows are still mapped and fully composited
  every frame, and their clients keep drawing. A background video player or game
  runs at full tilt.
- **Still mapped**, so screen-share pickers and window lists show invisible
  windows.
- **Channel availability is version-specific** — see below.

Channels **multiply** (`SMultiplyOperation`, identity `1.0`), so any single
channel at `0` forces the window invisible regardless of the others. That is
what lets us coexist with groups: we hold `WINDOW_ALPHA_MOVE_FROM_WORKSPACE` at
0 while the group drives `WINDOW_ALPHA_LAYOUT` per tab, and the product is
correct in every combination. Input likewise stays separate:
`MONOCLE_INACTIVE` (ours) and `GROUP_INACTIVE` (the group's) are distinct bits.

`WINDOW_ALPHA_MOVE_FROM_WORKSPACE` has **zero writers in 0.56.x**. It is written
once on `main` — see §10.

### A hybrid, if idle cost ever matters

Alpha for the transition, then `setHidden(true)` once the fade completes,
reversed on reveal. Gets the animation, the suspend, and the safety net. Needs
an animation-end callback, so only worth it if the idle CPU cost actually shows
up — measure with `hyprctl clients` against `top` before building it.

### Un-hide on every exit path

A hidden window with no layout to un-hide it is stranded forever, and with alpha
there is no fallback. Cover:

- `removeTarget()` — window leaving the layout
- destructor / `unhideAll()` — plugin unload or layout switch
- `swapTargets()` — target being replaced (§3)

Note `updateTiledAlgo`'s safeguard uses `TARGET->window()`, a single window, so
it would not have covered group members even if we did use `setHidden`.

---

## 5b. Some dispatchers are closed to plugins

`hl.dsp.window.cycle_next()` cannot drive a plugin layout. `Actions::cycleNext`
gates on a hardcoded typeid whitelist with no registration hook:

```cpp
constexpr const std::array<const std::type_info*, 2> LAYOUTS_WITH_CYCLE_NEXT = {
    &typeid(Layout::Tiled::CMonocleAlgorithm),
    &typeid(Layout::Tiled::CMasterAlgorithm),
};
```

Only those two route to `layoutMessage("cyclenext")`. The fallback path is no
help either: `windowState()->query().cycle(...)` filters on `acceptsInput()`,
which every input-blocked (or hidden) stack window fails, so it would only ever
bounce between master and slave.

Bind `hl.dsp.layout("cyclenext")` instead. This is **not** a workaround — it is
the exact call `cycle_next` makes internally for monocle. For the nicer call
shape, wrap it in Lua:

```lua
local function cycle_next(opts)
    local fwd = true
    if type(opts) == "table" and opts.next ~= nil then fwd = opts.next end
    return hl.dsp.layout(fwd and "cyclenext" or "cycleprev")
end

hl.bind("SUPER + Tab",         cycle_next())
hl.bind("SUPER + SHIFT + Tab", cycle_next({ next = false }))
```

General lesson: before assuming a built-in dispatcher will work, grep it for a
`typeid` whitelist or an `acceptsInput()` filter.

---

## 6. Lua config: `hyprctl keyword` is dead

Since 0.55 the config is Lua, and `hyprctl keyword` hard-fails for **every**
keyword:

```
keyword can't work with non-legacy parsers. Use eval.
```

Replacement is `hyprctl eval` calling `hl.config(...)`:

```sh
hyprctl eval "hl.config({ general = { layout = 'dwindle' } })"
```

In a bind, use Lua long brackets to dodge quote nesting:

```lua
hl.bind("SUPER + D", hl.dsp.exec_cmd([[hyprctl eval "hl.config({ general = { layout = 'dwindle' } })"]]))
```

`hl.config` walks the table, matches dotted keys against the internal config
registry, and — when called dynamically — calls `scheduleRefresh(refreshBits())`.
`general:layout` carries `REFRESH_LAYOUTS`, which triggers
`CWorkspaceAlgoMatcher::updateWorkspaceLayouts()`, the same path old `keyword`
used. Verify with `hyprctl activeworkspace | grep tiledLayout`.

### Plugin config values

Register in `PLUGIN_INIT` only, always under `plugin:`:

```cpp
auto v = makeShared<Config::Values::CFloatValue>(
    "plugin:twopanepersistent:mfact", "description", 0.5F,
    Config::Values::SFloatValueOptions{.min = 0.05F, .max = 0.95F});
HyprlandAPI::addConfigValueV2(PHANDLE, v);
```

Note `addConfigValue` (v1) is deprecated; use `addConfigValueV2` with typed
`Config::Values::*` objects. Read back with `->value()`.

Don't leave registered keys that no longer do anything — a stale key in the
user's config is either a silent no-op or an unknown-key error.

---

## 7. hyprpm packaging

- `hyprpm.toml` needs `[repository]` (`name`, `authors`) and a per-plugin table
  with **required** `output` and `build`.
- `build` commands run from the repo root; cwd resets between commands.
- **The Makefile's `SRC` paths must match the repo layout.** Flattening or
  nesting `src/` without updating `SRC` gives
  `No rule to make target 'main.cpp'`. This is a packaging error, not a compile
  error — it means make never even ran the compiler.
- `hyprpm add` prints `✔ installed repository` **even when the build failed.**
  Don't trust it; read the build lines above it, and use `-v`.
- Test the raw clone before blaming hyprpm:
  ```sh
  git clone <url> /tmp/test && cd /tmp/test && make all
  ```
- `commit_pins` are `["<hyprland commit>", "<your commit>"]` pairs. Empty means
  hyprpm always builds your latest commit against whatever Hyprland is
  installed — convenient while developing, fragile afterwards. Pin once working.
- Plugins **must** be built with GCC; clang mishandles the hook system.
- The `__hyprland_api_get_hash()` vs `__hyprland_api_get_client_hash()` check in
  `PLUGIN_INIT` is mandatory — mismatched headers crash rather than fail cleanly.

---

## 8. Code map

```
src/
├── globals.hpp             PHANDLE + config value handles
├── main.cpp                PLUGIN_INIT / PLUGIN_EXIT, registration
├── TwoPanePersistent.hpp   class decl
└── TwoPanePersistent.cpp   the layout
```

### State

| Member            | Meaning                                                                     |
| ----------------- | --------------------------------------------------------------------------- |
| `m_nodes`         | Targets in stack order. `m_nodes[0]` is master.                             |
| `m_slave`         | Remembered slave, `WP` so it self-invalidates. **The point of the layout.** |
| `m_split`         | Master fraction; instance-owned so resize/mfact are per-workspace.          |
| `m_splitSeeded`   | Whether `m_split` has been initialised from config yet.                     |
| `m_focusCallback` | Subscription to `window.active`.                                            |

### Control flow

```
newTarget      → insert below master, becomes slave → recalculate
removeTarget   → un-hide, drop node, clear m_slave if it was → recalculate
swapTargets    → 2-sided swap OR 1-sided replace (§3)        → recalculate
window.active  → onFocusChanged: index 0 → ignore; else m_slave = t → recalculate

recalculate:
  prune dead → fullscreen? bail to FS handler
  master = m_nodes[0], slave = resolveSlave()
  setWindowHidden(t, t != master && t != slave)   for all
  space()->recheckWorkArea()                      ← after visibility, before geometry
  1 node  → master gets whole workArea
  else    → master = left(m_split), everything else = right box
```

Hidden windows are parked at the **slave pane's geometry** so they are already
correctly sized the instant they are cycled into view.

`recheckWorkArea()` must be called _after_ changing visibility and _before_
reading `workArea()` — workspace rules like `w[tv1]` count visible windows.

### Haskell → C++

| Haskell                  | Here                                                                                                  |
| ------------------------ | ----------------------------------------------------------------------------------------------------- |
| `slaveWin :: Maybe a`    | `m_slave` (`WP<ITarget>`; expiry == `Nothing`)                                                        |
| `mFrac`                  | `m_split` ← `plugin:twopanepersistent:mfact`                                                          |
| `dFrac`                  | `plugin:twopanepersistent:dfact`                                                                      |
| `focusedMaster` fallback | `resolveSlave()`                                                                                      |
| `focusedSlave`           | `onFocusChanged()` — returns early at index 0, so master focus is a no-op. This _is_ the persistence. |
| `Shrink` / `Expand`      | `mfact -` / `mfact +`, plus `resizeTarget`                                                            |
| `down s`                 | `m_nodes[1..]`                                                                                        |

`cyclenext`/`cycleprev` = XMonad's `focusDown`/`focusUp`: they move focus across
the whole stack including master, and the focus listener promotes whatever lands
in focus into the slave pane.

---

## 9. Debugging checklist

Roughly the order these bit, and the order worth checking:

1. **Build error naming a source file** → `SRC` vs repo layout mismatch (§7).
2. **`No such file or directory` on a hyprland header** → version drift. Pull
   the exact tag and look, don't guess (§2).
3. **Windows vanish permanently** → a target left `m_nodes` without being
   un-hidden. Check `swapTargets` (§3) and every exit path (§5).
4. **Hidden window reappears on group tab switch** → alpha channel collision.
   Groups own `WINDOW_ALPHA_LAYOUT`; use a channel they do not write (§5).
5. **No animation when revealing a window** → `setHidden` has no transition;
   alpha does (§5).
6. **A built-in dispatcher silently does nothing** → check for a typeid
   whitelist (§5b).
7. **Config change does nothing** → `hyprctl keyword` is dead; use
   `eval` + `hl.config` (§6).
8. Develop in a nested session — plugin faults take the compositor with them.
   `hyprctl plugin unload <abs>.so ; hyprctl plugin load <abs>.so` is the reload
   cycle; note `PLUGIN_EXIT` is **not** called when the plugin is unloaded due
   to a fault.

---

## 10. Open / unverified

### Known `main`-only issue: `MOVE_FROM_WORKSPACE` is written there

`GlobalWindowController::moveToWorkspace` on `main` does:

```cpp
if (!WASVISIBLE && pWindow->m_workspace && pWindow->m_workspace->isVisible()) {
    pWindow->presentation().alpha(View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE)->setValueAndWarp(0.F);
    *pWindow->presentation().alpha(View::WINDOW_ALPHA_MOVE_FROM_WORKSPACE) = 1.F;
}
```

**Scope:** only `moveToWorkspace`, only when a not-currently-visible window
lands on a visible workspace. It runs _after_ `newTarget`, so it overwrites the
0 we just set.

**Symptom:** move a window into a `twopanepersistent` workspace where it belongs
in the hidden stack, and it fades in over the panes instead of staying hidden.

**Severity:** transient and self-correcting — the next `recalculate()` (any
focus change, cycle, new window, resize) puts it back to 0. Not corruption, not
permanent.

**Does not affect 0.56.x**, where the channel has zero writers.

**Deliberately not fixed yet.** Fixing it means deferring our alpha write past
Hyprland's, which needs a tick/idle scheduler — untested code working around
untested code, for a bug that cannot occur on the version we actually run. Do it
when someone is actually on `-git` and can reproduce and verify. At that point
re-check which channels are free rather than assuming this one still is; `main`
will have moved.

### Still open

- "Grid view" was never pinned down. The reported glitch was traced to §3 + §4
  and the group handling, but whether "grid view" means grouped tabs or
  something else (`hyprexpo`, an overview) has **not** been confirmed.
- `commit_pins` is still empty.
- The `main` compatibility path is reasoned from header diffs, not from a build.
  It is a compatibility _attempt_, not a supported configuration.
- Multi-monitor `moveTargetInDirection` fallback is implemented but untested.
- Idle CPU cost of alpha-hiding (vs `setHidden`'s suspend) has not been
  measured. If it is noticeable, see the hybrid note in §5.
