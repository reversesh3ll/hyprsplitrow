-- Switch or send focused window to primary
for i = 1, 10 do
  local index = i

  hl.bind("SUPER + F" .. tostring(index), function()
    hl.plugin.splitrow.showprimaryprofile(index)
  end)

  hl.bind("SUPER + SHIFT + F" .. tostring(index), function()
    hl.plugin.splitrow.sendprimaryprofile(index)
  end)
end

-- Switch or send focused window to secondary
for i = 1, 10 do
  local index = i

  hl.bind("SUPER + code:" .. tostring(index), function()
    hl.dispatch(hl.dsp.focus({ workspace = workspace_in_group(i) }))

  hl.bind("SUPER + SHIFT + code:" .. tostring(index), function()
    hl.plugin.splitrow.movetoworkspace(i)
  end)
end

-- Move focused window between primary and secondary
hl.bind("SUPER + SHIFT + Up", function()
  hl.plugin.splitrow.moveprimary()
end)

hl.bind("SUPER + SHIFT + Down", function()
  hl.plugin.splitrow.movesecondary()
end)

-- Set split axis and split ratio
hl.bind("SUPER + ALT + 1", function()
  hl.plugin.splitrow.setsplitaxis("horizontal")
  hl.plugin.splitrow.setsplitratio(1 / 3)
end)
hl.bind("SUPER + ALT + 2", function()
  hl.plugin.splitrow.setsplitaxis("horizontal")
  hl.plugin.splitrow.setsplitratio(1 / 2)
end)
hl.bind("SUPER + ALT + 3", function()
  hl.plugin.splitrow.setsplitaxis("horizontal")
  hl.plugin.splitrow.setsplitratio(2 / 3)
end)

-- Toggle pseudo/fake fullscreen on focused window
hl.bind("SUPER + D", function()
  hl.plugin.splitrow.togglefocusedfullscreen()
end)

-- Resize focused window
hl.bind("SUPER + ALT + Left", function()
  hl.plugin.splitrow.shrinkfocused()
end, { repeating = true })

hl.bind("SUPER + ALT + Right", function()
  hl.plugin.splitrow.growfocused()
end, { repeating = true })
