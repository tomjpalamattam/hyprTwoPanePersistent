# twopanepersistent

A Hyprland port of XMonad's `XMonad.Layout.TwoPanePersistent`.

Two panes: a master on the left and a slave on the right. Everything else on the
workspace is hidden. The slave pane **remembers** its window — when focus returns
to the master, the right pane does not swap itself out for whatever happens to be
next in the stack, which is the entire reason the XMonad module exists.

Requires Hyprland 0.55 or newer (it registers a tiled algorithm through
`HyprlandAPI::addTiledAlgo`, which replaced `IHyprLayout`).

Builds against both 0.56.x and current `-git`/`main`. `main` split `CWindow` into
`view/window/` with a `presentation()` sub-object; the source detects this with
`__has_include` and adapts, so one branch covers both.

## Install

```sh
hyprpm add https://github.com/<you>/hyprland-twopanepersistent
hyprpm enable twopanepersistent
hyprpm reload
```

## Config

```lua
config = {
    general = {
        layout = "twopanepersistent",
    },

    plugin = {
        twopanepersistent = {
            mfact        = 0.5,   -- master pane fraction (mFrac)
            dfact        = 0.03,  -- step for mfact +/- (dFrac)
        },
    },
}
```

## Binds

```lua
-- Tab cycles focus through the whole stack, master included, exactly like
-- XMonad's focusDown. Focusing a stack window moves it into the slave pane.
hl.bind("Tab",                hl.dsp.layout("cyclenext"))
hl.bind("SHIFT + Tab",        hl.dsp.layout("cycleprev"))

hl.bind(mainMod .. " + Return", hl.dsp.layout("swapwithmaster"))
hl.bind(mainMod .. " + m",      hl.dsp.layout("focusmaster"))

hl.bind(mainMod .. " + h",      hl.dsp.layout("mfact -"))
hl.bind(mainMod .. " + l",      hl.dsp.layout("mfact +"))
```

`mfact` also takes an explicit value: `mfact 0.6` sets it, `mfact +0.1` and
`mfact -0.1` adjust it. With no argument it resets to the configured `mfact`.

Dragging the divider with `resizeactive` works too — `resizeTarget` maps a
horizontal delta onto the split.

## layoutmsg commands

| command                   | effect                                                                 |
| ------------------------- | ---------------------------------------------------------------------- |
| `cyclenext` / `cycleprev` | move focus down / up the stack                                         |
| `swapwithmaster`          | promote the focused window to master; the old master becomes the slave |
| `focusmaster`             | focus the master pane                                                  |
| `mfact [+\|-\|<n>]`       | adjust the split                                                       |

## Mapping from the Haskell

| Haskell                        | here                                                                          |
| ------------------------------ | ----------------------------------------------------------------------------- |
| `slaveWin :: Maybe a`          | `m_slave` (a `WP<ITarget>`, so it self-invalidates when the window dies)      |
| `mFrac`                        | `m_split`, seeded from `plugin:twopanepersistent:mfact`                       |
| `dFrac`                        | `plugin:twopanepersistent:dfact`                                              |
| `focusedMaster` slave fallback | `resolveSlave()`                                                              |
| `focusedSlave`                 | `onFocusChanged()` — only fires for stack windows, so master focus is a no-op |
| `Shrink` / `Expand`            | `mfact -` / `mfact +`, plus `resizeTarget`                                    |

## Notes

- State is per workspace: each workspace gets its own instance with its own
  remembered slave and its own split.
- Hidden windows are parked at the slave pane's geometry so they are already the
  correct size the moment they are cycled into view.
- Hidden windows are un-hidden when removed from the layout and in the
  destructor, so switching layouts or unloading the plugin cannot strand a
  window at zero alpha.
