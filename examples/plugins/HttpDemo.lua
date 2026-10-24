-- HttpDemo: a background HTTP request. Shows plugin.http_request() with a
-- callback, plugin.cancel(), plugin.json_decode() and plugin.json_encode(),
-- and plugin.is_list_showing() before opening a screen from a late callback.

plugin.define({
    id = "example.http_demo",
    name = "HTTP Demo",
    version = "1.0",
    api_min = 14, -- is_list_showing() returns a handle from show_list()
})

local active = nil -- handle of the request in flight

local function fetch(menu)
    if active then
        plugin.show_toast("Already loading")
        return
    end
    local handle, err = plugin.http_request({
        url = "https://httpbin.org/json",
        headers = { Accept = "application/json" },
        connect_timeout_ms = 10000,
        total_timeout_ms = 20000,
    }, function(status, body, request_error)
        active = nil
        if request_error then
            plugin.show_toast("Failed: " .. request_error)
            return
        end
        local data, json_error = plugin.json_decode(body)
        if not data then
            plugin.show_toast("HTTP " .. status .. ", not JSON: " .. json_error)
            return
        end
        -- The user may have left the menu while the request ran.
        if not plugin.is_list_showing(menu) then return end
        local keys = {}
        for key in pairs(data) do keys[#keys + 1] = key end
        table.sort(keys)
        plugin.show_list("Response keys", keys, function(index)
            plugin.show_toast(plugin.json_encode(data[keys[index]]) or "?", 4000)
        end)
    end)
    if not handle then
        plugin.show_toast("Could not start: " .. err)
        return
    end
    active = handle
end

plugin.register_list_item("settings", "HTTP Demo", function()
    local menu
    menu = plugin.show_list("HTTP Demo", { "Fetch JSON", "Cancel" }, function(index)
        if index == 1 then
            fetch(menu)
        elseif active then
            plugin.show_toast(plugin.cancel(active) and "Cancelled" or "Already finished")
            active = nil
        end
    end)
end)
