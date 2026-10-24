-- ApiInfo: what this player and device support. Shows plugin.api_version(),
-- plugin.has_capability(), plugin.has_feature(), plugin.board() and
-- plugin.get_app_info(), shown on a plugin.show_details() page (or a plain
-- list on players without it).

plugin.define({
    id = "example.api_info",
    name = "API Info",
    version = "1.0",
    api_min = 1,
})

local CAPABILITIES = { "ui.settings", "ui.details", "network.http.async", "storage.secrets",
                       "library.paged", "radio.list", "filesystem.paths" }
local FEATURES = { "feature.airplay", "feature.dlna_renderer", "feature.remote", "feature.i18n" }

local function yes_no(b) return b and "yes" or "no" end

local function collect()
    local app = plugin.get_app_info()
    local sections = {
        { title = "Player", rows = {
            { "Version", app.version },
            { "Platform", app.platform },
            { "Plugin API", plugin.api_version() },
        } },
    }

    -- has_feature and board arrived later than the rest: check before use.
    if plugin.board then
        local b = plugin.board()
        local rows = {
            { "Board", b.name },
            { "Model", b.model },
            { "Screen", b.screen.w .. "x" .. b.screen.h },
        }
        for _, cap in ipairs(b.caps) do table.insert(rows, { cap, "yes" }) end
        table.insert(sections, { title = "Device", rows = rows })
    end

    local rows = {}
    for _, name in ipairs(CAPABILITIES) do
        table.insert(rows, { name, yes_no(plugin.has_capability(name)) })
    end
    if plugin.has_feature then
        for _, name in ipairs(FEATURES) do
            table.insert(rows, { name, yes_no(plugin.has_feature(name)) })
        end
    end
    table.insert(sections, { title = "Capabilities", rows = rows })
    return sections
end

plugin.register_list_item("settings", "API Info", function()
    local sections = collect()
    if plugin.has_capability("ui.details") then
        plugin.show_details("API Info", sections)
        return
    end
    local lines = {}
    for _, section in ipairs(sections) do
        for _, row in ipairs(section.rows) do
            table.insert(lines, row[1] .. ": " .. tostring(row[2]))
        end
    end
    plugin.show_list("API Info", lines, function() end)
end)
