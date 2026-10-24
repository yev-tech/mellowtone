-- SettingsDemo: a native-looking settings screen whose values survive a
-- restart. Shows plugin.show_settings_list() with a toggle, a slider and a
-- row that opens a nested screen, and plugin.storage for saving the values.

plugin.define({
    id = "example.settings_demo",
    name = "Settings Demo",
    version = "1.0",
    api_min = 2, -- plugin.storage
})

-- Storage keeps strings, so convert on the way in and out.
local enabled = plugin.storage.get("enabled", "true") == "true"
local level = tonumber(plugin.storage.get("level", "5")) or 5

local function show_about()
    plugin.show_settings_list("About", {
        { type = "row", label = "Enabled: " .. tostring(enabled), on_select = function() end },
        { type = "row", label = "Level: " .. level, on_select = function() end },
    })
end

plugin.register_list_item("settings", "Settings Demo", function()
    plugin.show_settings_list("Settings Demo", {
        { type = "toggle", label = "Enabled", value = enabled, on_change = function(on)
            enabled = on
            plugin.storage.set("enabled", tostring(on))
        end },
        { type = "slider", label = "Level", min = 0, max = 10, value = level, on_change = function(v)
            level = v
            plugin.storage.set("level", tostring(v))
        end },
        { type = "row", label = "About", on_select = show_about },
    })
end)
