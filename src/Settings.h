// Settings from plugins\TextureStreamer.lua
#pragma once

struct Settings
{
    bool debugWindow = false;      // console steals focus from exclusive fullscreen
    bool debugLog = false;         // usage/stall/fallback lines and TextureStreamer_boot.log
    bool patchDispatch = true;     // update budget clamps
    bool raiseBudget = true;       // false: hooks pass the game's own values through (monitor only)
    bool patchUpgrade = true;      // upgrades per frame
    bool smallPoolFallback = true; // failed small pool allocations retry in the large pool
    // per-hook switches, for isolating a crash
    bool hookPresent = true;
    bool hookUpdate = true;
    bool hookGetAvail = true;
    bool hookRequestConfig = true;
    bool lockVramMax = false; // report 0xFFFFFFFF to the game
    int upgradePerFrame = 32; // 16 to 127, game default 16
};

// Parses literal key = value lines, the file is never run.
void LoadSettings();
const Settings& GetSettings();
