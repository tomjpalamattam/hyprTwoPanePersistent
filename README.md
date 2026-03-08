# hyprTwoPanePersistent

An XMonad-style `TwoPanePersistent` layout plugin for Hyprland.

## Behavior

- **Master** always occupies the left pane
- **Whatever non-master window was last focused** occupies the right (slave) pane
- **When focus returns to master**, the slave pane does not change (this is the "persistent" part)
- **All other windows** are hidden in a `special:tpp_hidden` workspace and can be cycled into the slave pane

## Build

### Prerequisites

- Hyprland headers installed (`hyprland` package on Arch, or built from source)
- `cmake`, `pkg-config`, `g++` with C++23 support

### Steps

```bash
git clone <this repo>
cd hyprTwoPanePersistent
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The output is `build/libhyprTwoPanePersistent.so`.

### Rebuild after Hyprland updates

You **must** rebuild the plugin every time you update Hyprland. The ABI hash check at load time will tell you if you forgot.

## Installation

```bash
# Copy to a permanent location
mkdir -p ~/.config/hypr/plugins
cp build/libhyprTwoPanePersistent.so ~/.config/hypr/plugins/

# Load in hyprland.conf
plugin = ~/.config/hypr/plugins/libhyprTwoPanePersistent.so
```

## Configuration

```ini
# hyprland.conf

# Use this layout (can be per-workspace with 0.54+)
general {
    layout = TwoPanePersistent
}

# Or per-workspace:
# workspace = 1, layout:TwoPanePersistent

# Keybinds
$mod = SUPER

# Cycle the slave pane forward through hidden windows (like xmonad mod-tab)
bind = $mod, Tab,       tpp-cyclenext,
bind = $mod SHIFT, Tab, tpp-cycleprev,

# Focus master without disturbing the slave pane
bind = $mod, Left,  layoutmsg, focusmaster
bind = $mod, Right, layoutmsg, focusslave

# Resize the split
bind = $mod, H, layoutmsg, resizeactive -50 0
bind = $mod, L, layoutmsg, resizeactive 50 0
```

## How it maps to XMonad

| XMonad TwoPanePersistent | This plugin |
|---|---|
| `focusedSlave` → sets `slaveWin = focus s` | `onWindowFocused`: non-master focused → `pinnedSlave = pWindow` |
| `focusedMaster` with `Just win` → preserves slave | `onWindowFocused`: master focused → do nothing (slave stays) |
| `down s` exhausted (slave closed) | `onWindowRemovedTiling` → `promoteFromQueue()` |
| Stack cycling via `mod-tab` | `tpp-cyclenext` dispatcher / `layoutmsg cyclenext` |

## Troubleshooting

**"ABI mismatch" notification on load:**  
Rebuild the plugin against your current Hyprland version.

**Slave pane stays black after hiding:**  
The hidden workspace is `special:tpp_hidden`. This is normal — windows there are not rendered.

**Plugin not found:**  
Make sure the `.so` path in `hyprland.conf` is absolute.
