// Texture streamer budget hooks and patches
#include "pch.h"

#include "AddressSet.h"
#include "CodePatch.h"
#include "HookUtils.h"
#include "Settings.h"
#include "StallMonitor.h"
#include "SwapChainVram.h"
#include "TextureStreamerHooks.h"
#include "log.h"

namespace
{
    // Layout from retail 1.0.15.4 EN disassembly (FUN_14021dee0, FUN_1402a91d0, FUN_1402a9220).
    constexpr size_t kStreamerTsm = 0x26388;   // DgTextureStreamer -> texture storage manager
    constexpr size_t kTsmConfigApplied = 0x60; // {uint32 size, uint16 flag}
    constexpr size_t kTsmConfigPending = 0x68; // written by RequestTextureStorageConfiguration

    using Update_t = void(__fastcall*)(void* streamer);
    using RequestConfig_t = bool(__fastcall*)(void* tsm, uint32_t* config);
    using GetAvail_t = uint64_t(__fastcall*)();
    using AllocBlock_t = uint32_t(__fastcall*)(void* tsm, uint32_t size, int type);

    Update_t g_OrigUpdate = nullptr;
    RequestConfig_t g_OrigRequestConfig = nullptr;
    GetAvail_t g_OrigGetAvail = nullptr;
    AllocBlock_t g_OrigAllocBlock = nullptr;

    TextureStreamerState g_State = {};
    bool g_FirstUpdateSeen = false;
    uint32_t g_LastApplied = 0;
    bool g_Installed = false;

    // Target bytes, retail 1.0.15.4 EN, read in Ghidra 24/09/2026.
    const uint8_t kUpdateBytes[16] = { 0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8B, 0xEC, 0x48 };
    const uint8_t kGetAvailBytes[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0xE8 };
    const uint8_t kAllocBlockBytes[16] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x41 };
    const uint8_t kRequestConfigBytes[16] = { 0x8B, 0x02, 0x39, 0x41, 0x60, 0x75, 0x0D, 0x0F, 0xB7, 0x42, 0x04, 0x66, 0x39, 0x41, 0x64, 0x75 };

    const uint8_t kMovEbx3584MB[5] = { 0xBB, 0x00, 0x00, 0x00, 0xE0 }; // MOV EBX,0xE0000000
    const uint8_t kMovEbxMax[5] = { 0xBB, 0xFF, 0xFF, 0xFF, 0xFF };    // MOV EBX,0xFFFFFFFF, CMOVC after it saturates
    const uint8_t kCmovcR15dEax[4] = { 0x44, 0x0F, 0x42, 0xF8 };       // degrade flag when budget < 800MB
    const uint8_t kNop4[4] = { 0x90, 0x90, 0x90, 0x90 };
    const uint8_t kJzShort10[2] = { 0x74, 0x10 };
    const uint8_t kJmpShort10[2] = { 0xEB, 0x10 };
    const uint8_t kCmpUpgrade[7] = { 0x83, 0xBB, 0xB8, 0x00, 0x00, 0x00, 0x10 }; // CMP dword [RBX+0xB8],0x10
}

const TextureStreamerState& GetTextureStreamerState()
{
    return g_State;
}

// Budget sent to the game: adapter VRAM, only ever raises the game's value.
static uint32_t RaiseToVram(uint32_t gameValue)
{
    if (!GetSettings().raiseBudget)
        return gameValue;
    const uint32_t vram = Vram::GetReported();
    return vram > gameValue ? vram : gameValue;
}

// Asks the streamer to reconfigure to the VRAM budget once VRAM is known. The update
// function applies it through the game's own reconfigure path (state 5).
static void RequestVramBudget(void* streamer)
{
    if (g_State.requestedSize || !Vram::GetReported())
        return;

    uint8_t* tsm = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(streamer) + kStreamerTsm);
    if (!tsm)
        return;

    uint64_t config;
    memcpy(&config, tsm + kTsmConfigApplied, sizeof(config));
    const uint32_t current = static_cast<uint32_t>(config);
    const uint32_t wanted = RaiseToVram(current);
    g_State.requestedSize = wanted;

    if (wanted == current)
    {
        Log("[Streamer] Budget already %u MB, no request\n", current >> 20);
        return;
    }

    config = (config & 0xFFFFFFFF00000000ull) | wanted;
    const bool changed = g_OrigRequestConfig(tsm, reinterpret_cast<uint32_t*>(&config));
    Log("[Streamer] Requested budget %u MB (was %u MB) -> %s\n", wanted >> 20, current >> 20, changed ? "pending" : "unchanged");
}

// Logs the streamer budget each time the applied config changes, so the log shows the reconfigure landing.
// Offsets VERIFIED in FUN_14021a460 (retail 1.0.15.4 EN).
static void LogAppliedBudget(uint8_t* streamer)
{
    const uint8_t* tsm = *reinterpret_cast<uint8_t**>(streamer + kStreamerTsm);
    if (!tsm)
        return;
    const uint32_t applied = *reinterpret_cast<const uint32_t*>(tsm + kTsmConfigApplied);
    if (applied == g_LastApplied)
        return;
    g_LastApplied = applied;
    auto mb = [&](size_t off) { return *reinterpret_cast<const uint32_t*>(streamer + off) >> 20; };
    Log("[Streamer] Applied config %u MB: vramSize %u MB, clampedStore %u MB, storage %u MB, cacheBudget %u MB, degradeFlag %u\n",
        applied >> 20,
        mb(0x30a54),
        mb(0x30a64),
        mb(0x30a60),
        mb(0x30a70),
        static_cast<unsigned>(streamer[0x30a80]));
}

static void __fastcall hkUpdate(void* streamer)
{
    static bool traced = false;
    const bool first = !traced;
    traced = true;
    if (first)
        BootTrace("hkUpdate first call");
    if (streamer)
    {
        g_State.streamer = streamer;
        if (!g_FirstUpdateSeen)
        {
            g_FirstUpdateSeen = true;
            const void* tsm = *reinterpret_cast<void**>(static_cast<uint8_t*>(streamer) + kStreamerTsm);
            Log("[Streamer] First update: streamer=%p tsm=%p%s\n", streamer, tsm, tsm ? " (storage created before plugin load)" : "");
        }
        RequestVramBudget(streamer);
    }
    if (first)
        BootTrace("hkUpdate calling original");
    if (streamer)
        StallMonitor_UpdateBegin(static_cast<uint8_t*>(streamer));
    g_OrigUpdate(streamer);
    if (streamer)
        StallMonitor_UpdateEnd(static_cast<uint8_t*>(streamer));
    if (first)
        BootTrace("hkUpdate original returned");
    if (streamer)
        LogAppliedBudget(static_cast<uint8_t*>(streamer));
}

static bool __fastcall hkRequestConfig(void* tsm, uint32_t* config)
{
    static bool traced = false;
    if (!traced)
    {
        traced = true;
        BootTrace("hkRequestConfig first call");
    }
    if (!config)
        return g_OrigRequestConfig(tsm, config);

    uint64_t local;
    memcpy(&local, config, sizeof(local));
    const uint32_t gameValue = static_cast<uint32_t>(local);
    const uint32_t value = RaiseToVram(gameValue);
    if (value != gameValue)
    {
        Log("[Streamer] RequestTextureStorageConfiguration: %u MB -> %u MB\n", gameValue >> 20, value >> 20);
        local = (local & 0xFFFFFFFF00000000ull) | value;
    }
    return g_OrigRequestConfig(tsm, reinterpret_cast<uint32_t*>(&local));
}

// Game's own value until the first Present has resolved VRAM.
static uint64_t __fastcall hkGetAvail()
{
    static bool traced = false;
    if (!traced)
    {
        traced = true;
        BootTrace("hkGetAvail first call");
    }
    const uint32_t vram = GetSettings().raiseBudget ? Vram::GetReported() : 0;
    return vram ? vram : g_OrigGetAvail();
}

// TextureStorageManager::AllocBlock, FUN_1402a6d40. Type 0 uses the small pool (TSM+0x40), which the
// game sizes once at 0x10680000 before IH loads. When it is full the call returns 0xFFFFFFFF and the
// streamer (FUN_14021ab30) blocks until space frees. The type is the texture's quality level slot. Retry as type 2: large pool, handle | 0xAC920000.
// Address, size, free and move all pick the pool from the handle bits (FUN_1402a79f0, FUN_1402a7a20,
// FUN_1402a7680, FUN_1402a7e20), and FUN_1402a7970 reads 0xAC92 as block type 2, so the free in
// FUN_140219d80 takes the type 0 counters back off by the slot's own type.
static uint32_t __fastcall hkAllocBlock(void* tsm, uint32_t size, int type)
{
    const uint32_t handle = g_OrigAllocBlock(tsm, size, type);
    if (type >= 0 && type < 3)
    {
        if (handle == 0xFFFFFFFFu)
            ++g_State.allocFull[type];
        else if (handle == 0xFFFFFFFEu)
            ++g_State.allocGcWait[type];
    }
    if (type != 0 || handle != 0xFFFFFFFFu || !GetSettings().smallPoolFallback)
        return handle;

    const uint32_t retry = g_OrigAllocBlock(tsm, size, 2);
    if (retry < 0xFFFFFFFEu)
    {
        const uint32_t n = ++g_State.smallFallbacks;
        g_State.smallFallbackBytes += size;
        if (GetSettings().debugLog && (n <= 5 || n % 500 == 0))
            Log("[Storage] Small pool full, %u KB placed in the large pool (handle %08X, %u so far)\n", size >> 10, retry, n);
        return retry;
    }
    const uint32_t n = ++g_State.smallFallbackFails;
    if (GetSettings().debugLog && (n <= 5 || n % 500 == 0))
        Log("[Storage] Small pool full and large pool retry failed (%08X) for %u KB, %u so far\n", retry, size >> 10, n);
    return handle;
}

struct HookDef
{
    uintptr_t absAddr;
    const uint8_t* expected;
    size_t expectedSize;
    void* detour;
    void** original;
    const char* name;
    bool installed;
};

static HookDef g_Hooks[4];

bool Install_TextureStreamer_Hooks()
{
    if (g_Installed)
        return true;

    if (!AddressSetRuntime::HasAllAddresses())
    {
        Log("[Streamer] No address set for %s, nothing installed\n", GetGameBuildName(gGameBuild));
        return false;
    }

    const Settings& s = GetSettings();
    bool ok = true;

    // Update calls the original RequestTextureStorageConfiguration, so it installs last and only
    // if that hook went in.
    g_Hooks[0] = { gAddr.RequestTextureStorageConfiguration,
                   kRequestConfigBytes,
                   16,
                   (void*)&hkRequestConfig,
                   (void**)&g_OrigRequestConfig,
                   "RequestTextureStorageConfiguration",
                   false };
    g_Hooks[1] = {
        gAddr.GetAvailableStorageMemorySize, kGetAvailBytes, 16, (void*)&hkGetAvail, (void**)&g_OrigGetAvail, "GetAvailableStorageMemorySize", false
    };
    g_Hooks[2] = { gAddr.StreamerUpdate, kUpdateBytes, 16, (void*)&hkUpdate, (void**)&g_OrigUpdate, "StreamerUpdate", false };
    g_Hooks[3] = { gAddr.TsmAllocBlock, kAllocBlockBytes, 16, (void*)&hkAllocBlock, (void**)&g_OrigAllocBlock, "TextureStorageManager::AllocBlock", false };

    const bool enabled[4] = { s.hookRequestConfig, s.hookGetAvail, s.hookUpdate, s.smallPoolFallback };
    for (HookDef& h : g_Hooks)
    {
        if (!enabled[&h - g_Hooks])
        {
            Log("[Hook] %s: disabled in settings\n", h.name);
            continue;
        }
        if (&h == &g_Hooks[2] && !g_Hooks[0].installed)
        {
            Log("[Hook] %s: skipped, RequestTextureStorageConfiguration not hooked\n", h.name);
            ok = false;
            continue;
        }
        if (!CheckTargetBytes(h.name, h.absAddr, h.expected, h.expectedSize))
        {
            ok = false;
            continue;
        }
        h.installed = CreateAndEnableHook(ResolveGameAddress(h.absAddr), h.detour, h.original);
        Log("[Hook] %s: %s\n", h.name, h.installed ? "OK" : "FAIL");
        ok = ok && h.installed;
    }

    if (s.patchDispatch)
    {
        const CodePatch p[] = {
            { gAddr.UpdateAvailCap, kMovEbx3584MB, kMovEbxMax, 5, "3584MB cap -> uint32 max" },
            { gAddr.UpdateDegradeThreshold, kCmovcR15dEax, kNop4, 4, "degrade under 800MB" },
            { gAddr.UpdateForcedDegrade, kJzShort10, kJmpShort10, 2, "degrade on config flag" },
        };
        ok = ApplyPatchGroup("Update budget clamps", p, 3) && ok;
    }

    if (s.patchUpgrade)
    {
        // count at +0xB8 indexes a 64-entry array; the game also stops at 0x40, so 127 cannot overflow it
        const uint8_t patch[7] = { 0x83, 0xBB, 0xB8, 0x00, 0x00, 0x00, static_cast<uint8_t>(s.upgradePerFrame) };
        const CodePatch p[] = { { gAddr.ListupChangegradersUpgradeCap, kCmpUpgrade, patch, 7, "CMP [RBX+0xB8]" } };
        if (ApplyPatchGroup("doListupChangegraders upgrade per frame", p, 1))
            Log("[Streamer] Upgrade per frame: %d (was 16)\n", s.upgradePerFrame);
        else
            ok = false;
    }

    g_Installed = true;
    return ok;
}

bool Uninstall_TextureStreamer_Hooks()
{
    if (!g_Installed)
        return true;
    for (HookDef& h : g_Hooks)
    {
        if (h.installed)
            DisableAndRemoveHook(ResolveGameAddress(h.absAddr));
        h.installed = false;
    }
    g_Installed = false;
    return true;
}
