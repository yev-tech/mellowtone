-- Theme: a theme picker that loads .theme files. Shows the theming calls
-- (set_background_color, set_text_color, set_launcher_layout, set_icon),
-- plugin.refresh_theme() and plugin.reload_ui(), and re-applying a saved
-- choice from top-level code, since colors and layouts are not saved.
--
-- Install: copy Theme.lua to <SD card>/.mellowtone/plugins/ and the .theme
-- files to <SD card>/.mellowtone/plugins/Themes/. The picker is under
-- Settings > Display > Appearance > Theme.
--
-- .theme format, one key=value per line, '#' lines are comments:
--   name=Earthy                      shown in the picker (required)
--   screen= card= list_row=          background colors, 0xRRGGBB (required)
--   text_primary= text_muted=        text colors, 0xRRGGBB (required)
--   menu.<menu>.<field>=value        row style of one launcher menu (music,
--                                    stream_media or wireless). Fields:
--                                    bg_color, text_color, radius, row_gap,
--                                    height, width (numbers), align
--                                    (left|center|right), text_size
--                                    (small|medium|large|mono), accessory,
--                                    icon (true|false)
--   icon.<theme path>=<file>         replace an icon, e.g.
--                                    icon.launcher/music.png=earthy/music.png
--                                    (file relative to the Themes folder)

plugin.define({
    id = "example.theme",
    name = "Theme",
    version = "2.0",
    api_min = 8, -- refresh_theme, reload_ui, set_launcher_layout
})

local state_root = plugin.paths and plugin.paths().state_root or (plugin.sd_root() .. "/.mellowtone")
local THEMES_DIR = state_root .. "/plugins/Themes"
local COLORS = { "screen", "card", "list_row", "text_primary", "text_muted" }
local MENUS = { music = true, stream_media = true, wireless = true }
local MENU_NUMBERS = { bg_color = true, text_color = true, radius = true, row_gap = true, height = true, width = true }
local MENU_STRINGS = { align = true, text_size = true }
local MENU_BOOLEANS = { accessory = true, icon = true }

-- Reads one .theme file into a table, or nil if it is incomplete.
local function load_theme(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local t = { menus = {}, icons = {} }
    for line in f:lines() do
        local menu, field, value = line:match("^menu%.([%w_]+)%.([%w_]+)=(.*)$")
        if menu and MENUS[menu] then
            local style = t.menus[menu] or {}
            t.menus[menu] = style
            if MENU_NUMBERS[field] then
                style[field] = tonumber(value)
            elseif MENU_STRINGS[field] then
                style[field] = value
            elseif MENU_BOOLEANS[field] then
                style[field] = (value == "true")
            end
        else
            local icon, file = line:match("^icon%.(.-)=(.*)$")
            if icon then
                t.icons[icon] = THEMES_DIR .. "/" .. file
            else
                local key
                key, value = line:match("^([%w_]+)=(.*)$")
                if key then t[key] = value end
            end
        end
    end
    f:close()
    if not t.name then return nil end
    for _, c in ipairs(COLORS) do
        t[c] = tonumber(t[c])
        if not t[c] then return nil end
    end
    return t
end

local function scan_themes()
    local themes = {}
    for _, e in ipairs(plugin.list_dir(THEMES_DIR)) do
        if not e.dir and e.name:match("%.theme$") then
            local t = load_theme(THEMES_DIR .. "/" .. e.name)
            if t then
                t.file = e.name
                table.insert(themes, t)
            end
        end
    end
    table.sort(themes, function(a, b) return a.name < b.name end)
    return themes
end

local function apply(t)
    plugin.set_background_color("screen", t.screen)
    plugin.set_background_color("card", t.card)
    plugin.set_background_color("list_row", t.list_row)
    plugin.set_text_color("primary", t.text_primary)
    plugin.set_text_color("muted", t.text_muted)
    -- Replaces any earlier launcher layout; {} resets all three menus.
    plugin.set_launcher_layout(t.menus)
    -- Copied icons stay on the device until replaced again.
    for icon, file in pairs(t.icons) do pcall(plugin.set_icon, icon, file) end
end

-- Top-level code runs on every load: re-apply the saved choice.
local selected = plugin.storage.get("theme", "")
for _, t in ipairs(scan_themes()) do
    if t.file == selected then pcall(apply, t) end
end

plugin.register_list_item("display", "Theme", function()
    local themes = scan_themes()
    local labels, current = { "Default" }, 1
    for i, t in ipairs(themes) do
        labels[i + 1] = t.name
        if t.file == selected then current = i + 1 end
    end
    plugin.show_list("Theme", labels, function(index)
        local t = themes[index - 1]
        selected = t and t.file or ""
        plugin.storage.set("theme", selected)
        if t then
            local ok, err = pcall(apply, t)
            if not ok then plugin.show_toast("Theme: " .. tostring(err)) end
            plugin.refresh_theme() -- applies icons and the launcher layouts
        else
            plugin.reload_ui() -- restarts plugins, back to the default look
        end
    end, { selected = current })
end, { group = "appearance" })
