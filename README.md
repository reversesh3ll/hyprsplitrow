# hyprsplitrow

`hyprsplitrow` is a Hyprland layout plugin for large single-monitor setups (such as ultrawide monitors) where one physical display behaves like two independent regions.

Choose a split direction with `setsplitaxis`:

- **horizontal**: primary region at the top, secondary region at the bottom
- **vertical**: primary region on the left, secondary region on the right

Primary profiles are independent from normal workspace switching. The secondary region follows normal Hyprland workspace switching. The layout uses only `primary` and `secondary` terminology.

## What problem does it solve?

Hyprland normally treats one monitor as one workspace area. On a large display, it is useful to keep one region stable while another follows normal workspace changes.

`hyprsplitrow` provides this without relying on fake outputs, pinned-window rules, or special workspace workarounds that can interfere with focus, placement, fullscreen, and workspace movement.

## Features

- Horizontal top/bottom or vertical left/right monitor split.
- Primary profiles independent from workspace switching.
- Secondary region follows normal Hyprland workspaces.
- Move, reorder, and resize windows within a region.
- Move windows between regions.
- Region-local pseudo-fullscreen.
- Configurable primary-region ratio.
- Ten primary profiles.
- Focus-aware window placement and profile reveal.
- Runtime persistence for order, resize weights, and pseudo-fullscreen state.

## Split direction

Horizontal is the default:

```lua
hl.plugin.splitrow.setsplitaxis("horizontal")
```

For a left/right split on an ultrawide or wide display:

```lua
hl.plugin.splitrow.setsplitaxis("vertical")
```

Changing the axis updates the layout immediately and is saved in the plugin state file.

## Install with hyprpm

```bash
hyprpm add https://github.com/reversesh3ll/hyprsplitrow
hyprpm enable hyprsplitrow
hyprpm reload
```

To load enabled plugins automatically on Hyprland startup:

```lua
hl.on("hyprland.start", function()
  hl.exec_cmd("hyprpm reload")
end)
```

If Hyprland permission management is enabled:

```lua
hl.permission("/usr/(bin|local/bin)/hyprpm", "plugin", "allow")
```

## Upgrading from 0.1

Version 0.2 renames the split regions and adds vertical left/right mode.

This is a breaking change. Update your Lua config and keybinds before loading the new plugin.

| Old command | New command |
| --- | --- |
| `movetop()` | `moveprimary()` |
| `movebottom()` | `movesecondary()` |
| `togglerow()` | `toggleregion()` |
| `settoprowratio(ratio)` | `setsplitratio(ratio)` |
| `showprofile(index)` | `showprimaryprofile(index)` |
| `sendtoprofile(index)` | `sendprimaryprofile(index)` |

The old commands have been removed. No compatibility aliases are provided.

The state cache has also changed. Version 0.2 uses:

```text
~/.cache/hyprsplitrow-primary-secondary/state.txt
```

## Basic configuration

The layout name remains `splitrow` on this branch:

```lua
hl.config({
  general = {
    layout = "splitrow",
  },
})
```

Set the primary-region ratio:

```lua
hl.plugin.splitrow.setsplitratio(1 / 3)
hl.plugin.splitrow.setsplitratio(1 / 2)
hl.plugin.splitrow.setsplitratio(2 / 3)
```

In horizontal mode, the ratio controls the primary region height. In vertical mode, it controls the primary region width.

## Required commands and suggested keybinds

Most commands need bindings for the layout to work as intended. The key choices below are only examples.

### Move windows between regions

```lua
hl.bind("SUPER + SHIFT + Up", function()
  hl.plugin.splitrow.moveprimary()
end)

hl.bind("SUPER + SHIFT + Down", function()
  hl.plugin.splitrow.movesecondary()
end)

hl.bind("SUPER + grave", function()
  hl.plugin.splitrow.toggleregion()
end)
```

### Region-local pseudo-fullscreen

```lua
hl.bind("SUPER + D", function()
  hl.plugin.splitrow.togglefocusedfullscreen()
end)
```

### Resize the focused window

```lua
hl.bind("SUPER + ALT + Left", function()
  hl.plugin.splitrow.shrinkfocused()
end, { repeating = true })

hl.bind("SUPER + ALT + Right", function()
  hl.plugin.splitrow.growfocused()
end, { repeating = true })
```

### Reorder windows inside their region

```lua
hl.bind("SUPER + SHIFT + Left", function()
  hl.plugin.splitrow.moveleft()
end)

hl.bind("SUPER + SHIFT + Right", function()
  hl.plugin.splitrow.moveright()
end)
```

### Change split ratio

```lua
hl.bind("SUPER + ALT + 1", function()
  hl.plugin.splitrow.setsplitratio(1 / 3)
end)

hl.bind("SUPER + ALT + 2", function()
  hl.plugin.splitrow.setsplitratio(1 / 2)
end)

hl.bind("SUPER + ALT + 3", function()
  hl.plugin.splitrow.setsplitratio(2 / 3)
end)
```

### Switch primary profiles and assign windows

```lua
for i = 1, 10 do
  local index = i

  hl.bind("SUPER + F" .. tostring(index), function()
    hl.plugin.splitrow.showprimaryprofile(index)
  end)

  hl.bind("SUPER + SHIFT + F" .. tostring(index), function()
    hl.plugin.splitrow.sendprimaryprofile(index)
  end)
end
```

### Move windows to normal workspaces

Use the plugin command so windows in primary profiles are released correctly before the native workspace move:

```lua
for i = 1, 10 do
  local numberkey = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 }
  local index = i

  hl.bind("SUPER + SHIFT + code:" .. numberkey[i], function()
    hl.plugin.splitrow.movetoworkspace(index)
  end)
end
```

## Commands

```lua
hl.plugin.splitrow.moveprimary()
hl.plugin.splitrow.movesecondary()
hl.plugin.splitrow.toggleregion()

hl.plugin.splitrow.moveleft()
hl.plugin.splitrow.moveright()

hl.plugin.splitrow.shrinkfocused()
hl.plugin.splitrow.growfocused()

hl.plugin.splitrow.togglefocusedfullscreen()
hl.plugin.splitrow.setsplitratio(1 / 3)
hl.plugin.splitrow.setsplitaxis("vertical")

hl.plugin.splitrow.showprimaryprofile(1)
hl.plugin.splitrow.sendprimaryprofile(1)
hl.plugin.splitrow.movetoworkspace(1)
```

Valid primary profile numbers are `1` through `10`.

## Requirements

- Hyprland with plugin support.
- Hyprland headers matching the running Hyprland build.
- `make` and `pkg-config`.
- A C++ compiler supporting the C++ version required by your Hyprland headers.

## Manual build

```bash
git clone https://github.com/reversesh3ll/hyprsplitrow.git
cd hyprsplitrow
make clean
make

hyprctl plugin load "$PWD/hyprsplitrow.so"
hyprctl reload
```

Unload it with:

```bash
hyprctl plugin unload "$PWD/hyprsplitrow.so"
```

## State file

This branch uses a separate state file and does not read the previous directional state format:

```text
$XDG_CACHE_HOME/hyprsplitrow-primary-secondary/state.txt
```

Fallback path:

```text
~/.cache/hyprsplitrow-primary-secondary/state.txt
```

The file uses state format version `5` and persists plugin reload state only within the same Hyprland process. Version 4 state from the rename branch is read as a horizontal split.

## Known limitations

- Regions use a simple column layout in both split directions.
- Primary profiles are available only in the primary region.
- Primary profiles are not real Hyprland workspaces.
- Pseudo-fullscreen is not native Hyprland fullscreen.
- State persistence is not a full session restore system.
- Hyprland plugin internals can change between Hyprland releases.
