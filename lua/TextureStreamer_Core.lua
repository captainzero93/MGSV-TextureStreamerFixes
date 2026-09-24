-- TextureStreamer IH loader
local this = {}

if rawget(_G, "TextureStreamer_Core") then
    return _G.TextureStreamer_Core
end

local DLL_NAME = "TextureStreamer"

-- Rewrites the whole loader log with "w" on each call, like InfCore.WriteLog. The game's io.open
-- only honours "w" (other modes open read-only), and file:flush corrupts the handle, so neither
-- "a" nor flush is used (retail 1.0.15.4 EN, fopen 0x141A62B80, flush 0x141A30930).
local logLines = {}
local function LoaderLog(gamePath, msg)
    InfCore.Log("TextureStreamer_Core: " .. msg)
    logLines[#logLines + 1] = msg
    local f = io.open(gamePath .. "plugins/" .. DLL_NAME .. "_loader.log", "w")
    if f then
        f:write(table.concat(logLines, "\n") .. "\n")
        f:close()
    end
end

local function Load()
    local gamePath = InfCore.gamePath
    if not gamePath then
        InfCore.Log("TextureStreamer_Core: InfCore.gamePath is nil, not loading")
        return
    end

    local dllPath = string.gsub(gamePath .. "plugins/" .. DLL_NAME .. ".dll", "/", "\\")
    LoaderLog(gamePath, "loading " .. dllPath)

    if not package or not package.loadlib then
        LoaderLog(gamePath, "ERROR: package.loadlib not available")
        return
    end

    local open, err = package.loadlib(dllPath, "luaopen_" .. DLL_NAME)
    if not open then
        LoaderLog(gamePath, "ERROR: loadlib failed: " .. tostring(err))
        return
    end

    LoaderLog(gamePath, "loadlib returned, calling luaopen_" .. DLL_NAME)
    local ok, callErr = pcall(open)
    if ok then
        LoaderLog(gamePath, "loaded, see " .. DLL_NAME .. ".log beside the game exe")
    else
        LoaderLog(gamePath, "ERROR: luaopen_" .. DLL_NAME .. " failed: " .. tostring(callErr))
    end
end

Load()

_G.TextureStreamer_Core = this

return this
