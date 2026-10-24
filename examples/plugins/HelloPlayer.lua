-- HelloPlayer: the smallest useful plugin.
-- Shows plugin.define(), a row in Settings > System > Additional Tools,
-- a list screen opened from it, and a toast.

plugin.define({
    id = "example.hello",
    name = "Hello Player",
    version = "1.0",
    api_min = 1,
})

local fruits = { "Apple", "Banana", "Cherry" }

plugin.register_list_item("settings", "Hello Player", function()
    plugin.show_list("Hello Player", fruits, function(index)
        plugin.show_toast("You picked " .. fruits[index], 2000)
    end)
end)
