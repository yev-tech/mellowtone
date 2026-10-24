-- PlaybackEvents: reacting to the player. Shows plugin.on() events, a
-- plugin.set_interval() timer, reading the playback state and driving
-- playback from a settings row.

plugin.define({
    id = "example.playback_events",
    name = "Playback Events",
    version = "1.0",
    api_min = 1,
})

local played = 0

plugin.on("track_started", function(title, artist)
    played = played + 1
    plugin.show_toast(("Now playing: %s - %s"):format(artist or "?", title or "?"), 2000)
end)

plugin.on("paused", function() print("[PlaybackEvents] paused") end)
plugin.on("resumed", function() print("[PlaybackEvents] resumed") end)

-- Every 60 seconds, log where the current track is.
plugin.set_interval(60, function()
    if plugin.is_playing() then
        print(("[PlaybackEvents] %.0f / %.0f s"):format(plugin.get_position(), plugin.get_duration()))
    end
end)

plugin.register_list_item("playback", "Playback Events", function()
    local title, artist = plugin.get_now_playing()
    plugin.show_settings_list("Playback Events", {
        { type = "row", label = "Now: " .. (title and (artist .. " - " .. title) or "nothing"),
          on_select = function() end },
        { type = "row", label = "Tracks started: " .. played, on_select = function() end },
        { type = "row", label = "Play / Pause", on_select = plugin.toggle_pause },
        { type = "row", label = "Next track", on_select = plugin.next_track },
        { type = "row", label = "Restart track", on_select = function() plugin.seek(0) end },
    })
end)
