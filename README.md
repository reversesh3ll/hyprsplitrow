## Disclaimer

This is a hobby project I created for my own personal use. But I am very happy with it, so thought others may find it useful. AI was used extensively during the development of this plugin.

# hyprsplitrow

`hyprsplitrow` is a Hyprland layout plugin for large single-monitor setups where you want one physical display to behave like two horizontal virtual monitors.

It was built for setups where each row of the layout should act like its own work area. The top row (profiles) switch independent from normal workspace switching, while the bottom row follows normal Hyprland workspace switching. In practice, it feels about 95% similar to the old X11-style virtual split monitor workflow, but implemented as a Hyprland layout plugin instead of a fake monitor/output configuration.

This is intended for large single-monitor setups, especially large 4K displays. Side-by-side ultrawide splitting is not supported yet. That may be added later if there is interest.

## What problem does it solve?

Hyprland normally treats one physical monitor as one workspace area. On very large single displays, that can be awkward if you want part of the screen to stay independent while the rest switches between normal workspaces.

Common examples:

- Keep chat, docs, monitoring tools, or a browsers visible in the top row or vise versa.
- Switch normal workspaces in the bottom row.
- Move windows between the two rows with keybinds.
- Fullscreen a window inside only its row instead of taking over the whole monitor.

You can fake parts of this with pinned windows, special workspaces, or manual window rules, but those approaches tend to break focus, z-order, placement, fullscreen, tooltips, animations, or workspace switching. 
`hyprsplitrow` handles this as an active layout instead.

## Features

- Splits one monitor into a top row and bottom row.
- Both rows are tiled column workspaces.
- Pseudo-fullscreen window inside each row.
- Custom top/bottom ratios. (defaults to one third of the monitor height)
- Live update the top/bottom ratio.
- Ten top profiles (default).
- Top profiles are independent from normal workspace switching.
- Bottom row follows normal Hyprland workspace switching.
- Move focused windows between top and bottom rows.
- Move windows left/right within their current row.
- Resize windows within their current row.
- Top profile switching uses a subtle fade animation.
- New windows spawn next to the current focused window.
- Runtime state persistence for window ordering, resize weights, fullscreen state.

## Install with hyprpm

Add the repository:

```bash
hyprpm add https://github.com/reversesh3ll/hyprsplitrow
```

Enable the plugin:

```bash
hyprpm enable hyprsplitrow
```

Load enabled plugins:

```bash
hyprpm reload
```

To load enabled hyprpm plugins automatically on Hyprland startup, add this to your Lua config:

```lua
hl.on("hyprland.start", function()
  hl.exec_cmd("hyprpm reload")
end)
```

If you use Hyprland permission management, allow hyprpm to load plugins:

```lua
hl.permission("/usr/(bin|local/bin)/hyprpm", "plugin", "allow")
```

## Basic configuration

Set the Hyprland layout to `splitrow`:

```lua
hl.config({
  general = {
    layout = "splitrow",
  },
})
```

Optional row ratio presets:

```lua
hl.plugin.splitrow.settoprowratio(1 / 3) -- top third, bottom two thirds
hl.plugin.splitrow.settoprowratio(1 / 2) -- equal split
hl.plugin.splitrow.settoprowratio(2 / 3) -- top two thirds, bottom third
```

## Keybinds

Most plugin commands are expected to have keybinds for the layout to feel correct. The exact keys do not matter. The examples below are only suggested bindings.

### Note

I bind the number keys for normal workspaces and the F1-F10 keys for top row profiles. Most issues you may experience are related to not using these plugin commands on your keybinds.

### Move windows between rows

```lua
hl.bind("SUPER + SHIFT + Up", function()
  hl.plugin.splitrow.movetop()
end)

hl.bind("SUPER + SHIFT + Down", function()
  hl.plugin.splitrow.movebottom()
end)

hl.bind("SUPER + grave", function()
  hl.plugin.splitrow.togglerow()
end)
```

### Pseudo-fullscreen inside the current row

```lua
hl.bind("SUPER + D", function()
  hl.plugin.splitrow.togglefocusedfullscreen()
end)
```

### Resize the focused window within its row

```lua
hl.bind("SUPER + ALT + Left", function()
  hl.plugin.splitrow.shrinkfocused()
end, { repeating = true })

hl.bind("SUPER + ALT + Right", function()
  hl.plugin.splitrow.growfocused()
end, { repeating = true })
```

### Move the focused window within its row

```lua
hl.bind("SUPER + SHIFT + Left", function()
  hl.plugin.splitrow.moveleft()
end)

hl.bind("SUPER + SHIFT + Right", function()
  hl.plugin.splitrow.moveright()
end)
```

### Change row ratio

```lua
hl.bind("SUPER + ALT + 1", function()
  hl.plugin.splitrow.settoprowratio(1 / 3)
end)

hl.bind("SUPER + ALT + 2", function()
  hl.plugin.splitrow.settoprowratio(1 / 2)
end)

hl.bind("SUPER + ALT + 3", function()
  hl.plugin.splitrow.settoprowratio(2 / 3)
end)
```

### Switch top profiles and send windows to top profiles

```lua
for i = 1, 10 do
  local index = i

  hl.bind("SUPER + F" .. tostring(index), function()
    hl.plugin.splitrow.showprofile(index)
  end)

  hl.bind("SUPER + SHIFT + F" .. tostring(index), function()
    hl.plugin.splitrow.sendtoprofile(index)
  end)
end
```

### Move windows to normal workspaces

Use the plugin helper instead of Hyprland's normal workspace move command when you want this to work correctly for top-profile windows:

```lua
for i = 1, 10 do
  local numberkey = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 }
  local index = i

  hl.bind("SUPER + SHIFT + code:" .. numberkey[i], function()
    hl.plugin.splitrow.movetoworkspace(index)
  end)
end
```

This releases a top-profile window from splitrow state before moving it to the target normal workspace.

## Requirements (build)

- Hyprland with plugin support.
- Hyprland headers installed.
- `make`.
- `pkg-config`.
- A C++ compiler that supports the C++ standard required by your Hyprland headers.

For hyprpm users, make sure the usual Hyprland plugin build dependencies are installed. Hyprland's plugin documentation lists `cpio`, `cmake`, `git`, `meson`, and `gcc` as common requirements for hyprpm plugin builds.

## Manual build

Clone the repository:

```bash
git clone https://github.com/reversesh3ll/hyprsplitrow.git
cd hyprsplitrow
```

Build the plugin:

```bash
make clean
make
```

The build should produce:

```text
hyprsplitrow.so
```

Load it manually:

```bash
hyprctl plugin load "$PWD/hyprsplitrow.so"
hyprctl reload
```

Unload it manually:

```bash
hyprctl plugin unload "$PWD/hyprsplitrow.so"
```

The path passed to `hyprctl plugin load` must be absolute. `$PWD/hyprsplitrow.so` is fine because the shell expands it to an absolute path.

## Commands

### Row commands

```lua
hl.plugin.splitrow.movetop()
hl.plugin.splitrow.movebottom()
hl.plugin.splitrow.togglerow()
```

### Row-local movement

```lua
hl.plugin.splitrow.moveleft()
hl.plugin.splitrow.moveright()
```

### Row-local resizing

```lua
hl.plugin.splitrow.shrinkfocused()
hl.plugin.splitrow.growfocused()
```

### Pseudo-fullscreen

```lua
hl.plugin.splitrow.togglefocusedfullscreen()
```

### Top row ratio

```lua
hl.plugin.splitrow.settoprowratio(1 / 3)
hl.plugin.splitrow.settoprowratio(1 / 2)
hl.plugin.splitrow.settoprowratio(2 / 3)
```

### Top profiles

```lua
hl.plugin.splitrow.showprofile(1)
hl.plugin.splitrow.sendtoprofile(1)
```

Valid profile numbers are `1` through `10`.

### Workspace movement

```lua
hl.plugin.splitrow.movetoworkspace(1)
```

This is the preferred command for moving a focused top-profile window into a normal workspace.

## State file

The plugin stores runtime state here:

```text
~/.cache/hyprsplitrow/state.txt
```

The state file is used for plugin reload persistence inside the same Hyprland process.

It stores things like:

- active top profile
- top profile membership
- top profile order
- top profile resize weights
- top pseudo-fullscreen state
- bottom row order
- bottom row resize weights
- bottom pseudo-fullscreen state

The state file is not intended to be a long-term session restore mechanism across full Hyprland restarts.

## Development workflow

Recommended local workflow:

```bash
make clean
make
hyprctl plugin unload "$PWD/hyprsplitrow.so"
hyprctl plugin load "$PWD/hyprsplitrow.so"
hyprctl reload
```

Useful checks:

```bash
hyprctl plugin list
hyprctl -j clients
cat ~/.cache/hyprsplitrow/state.txt
```

## Known limitations

- Only horizontal top/bottom splitting is supported.
- Side-by-side ultrawide splitting is not supported yet.
- Rows currently use a simple column layout.
- Top profiles are not real Hyprland workspaces.
- This is a plugin-level layout, not a true Hyprland core monitor split.
- Fullscreen is implemented as row-local pseudo-fullscreen, not native Hyprland fullscreen.
- State persistence is for plugin reloads inside the same Hyprland process. It is not a full session restore system.
- Hyprland plugins use internal APIs, so the plugin may need updates when Hyprland changes.
