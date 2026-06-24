for i = 1, 10 do
  local numberkey = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 }

  hl.bind("SUPER + SHIFT + code:" .. numberkey[i], function()
    hl.plugin.splitrow.movetoworkspace(i)
  end)
end

hl.bind("SUPER + SHIFT + Up", function()
  hl.plugin.splitrow.moveprimary()
end)

hl.bind("SUPER + SHIFT + Down", function()
  hl.plugin.splitrow.movesecondary()
end)

for i = 1, 10 do
  local index = i

  hl.bind("SUPER + F" .. tostring(index), function()
    hl.plugin.splitrow.showprimaryprofile(index)
  end)

  hl.bind("SUPER + SHIFT + F" .. tostring(index), function()
    hl.plugin.splitrow.sendprimaryprofile(index)
  end)
end

for i = 1, 10 do
    local numberkey = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19 }
    hl.unbind("SUPER + ALT + code:" .. numberkey[i])
end
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

hl.unbind("SUPER + D")
hl.bind("SUPER + D", function()
  hl.plugin.splitrow.togglefocusedfullscreen()
end)

hl.bind("SUPER + ALT + Left", function()
  hl.plugin.splitrow.shrinkfocused()
end, { repeating = true })

hl.bind("SUPER + ALT + Right", function()
  hl.plugin.splitrow.growfocused()
end, { repeating = true })
