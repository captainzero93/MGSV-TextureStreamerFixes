// Frame stall and streamer usage logging
#include "pch.h"

#include <atomic>

#include "Settings.h"
#include "StallMonitor.h"
#include "TextureStreamerHooks.h"
#include "log.h"

namespace
{
    constexpr double kUpdateStallMs = 50.0; // streamer update slower than this is logged
    constexpr double kFrameGapMs = 250.0;   // gap between Presents longer than this is logged
    constexpr double kSnapshotEveryMs = 5000.0;

    std::atomic<uint8_t*> g_Streamer{ nullptr };
    std::atomic<int64_t> g_UpdateStart{ 0 }; // QPC tick while an update runs, 0 otherwise
    int64_t g_LastPresent = 0;
    int64_t g_LastSnapshot = 0;
    double g_TicksPerMs = 0.0;

    int64_t Now()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (g_TicksPerMs == 0.0)
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            g_TicksPerMs = static_cast<double>(f.QuadPart) / 1000.0;
        }
        return t.QuadPart;
    }

    double Ms(int64_t from, int64_t to)
    {
        return static_cast<double>(to - from) / g_TicksPerMs;
    }

    uint32_t U32(const uint8_t* p, size_t off)
    {
        return *reinterpret_cast<const uint32_t*>(p + off);
    }

    // Storage manager block allocator free MB: total - (top + 1) * blockSize.
    // Layout from dev decomp GetSmallFreeSize/GetLargeFreeSize (2015 prototype); retail not checked.
    uint32_t FreeMb(const uint8_t* tsm, size_t allocOff, uint32_t* totalMb)
    {
        const uint8_t* a = *reinterpret_cast<uint8_t* const*>(tsm + allocOff);
        if (!a)
        {
            *totalMb = 0;
            return 0;
        }
        const uint32_t total = U32(a, 0x8);
        const uint32_t block = U32(a, 0xC);
        const uint32_t top = U32(a, 0x18);
        *totalMb = total >> 20;
        const uint64_t used = static_cast<uint64_t>(top + 1) * block;
        return used >= total ? 0 : static_cast<uint32_t>((total - used) >> 20);
    }
}

// Retail 1.0.15.4 EN streamer offsets, checked in Ghidra:
// +0x26388 tsm, +0x26328 update state, +0x30a60 storage, +0x30a70 cache budget, +0x30a80 degrade flag,
// +0x729f8 release list count (FUN_140222010).
// +0x30a74/78/7c bytes of textures by the level they currently show (FUN_14021f9a0, FUN_140222010).
// +0x3efa0/a4/a8 blocks and +0x3efac/b0/b4 bytes allocated per level slot (FUN_14021ab30 alloc,
// FUN_140219d80 free). +0x3efc4/c8 blocks and bytes freed as block type 3. The update zeroes the
// per-frame fail counters +0x3efb8..c0, so fails are counted in the AllocBlock hook instead.
// +0x69174 search tags is from older sources, not checked.
static void LogSnapshot(const char* reason)
{
    const uint8_t* s = g_Streamer.load();
    if (!s)
    {
        Log("[Usage] %s: streamer not captured yet\n", reason);
        return;
    }
    const uint8_t* tsm = *reinterpret_cast<uint8_t* const*>(s + 0x26388);
    uint32_t smallTotal = 0;
    uint32_t largeTotal = 0;
    const uint32_t smallFree = tsm ? FreeMb(tsm, 0x40, &smallTotal) : 0;
    const uint32_t largeFree = tsm ? FreeMb(tsm, 0x48, &largeTotal) : 0;
    const TextureStreamerState& ts = GetTextureStreamerState();

    Log("[Usage] %s: state %u, storage %u MB, cacheBudget %u MB, degrade %u, small free %u/%u MB, large free %u/%u MB, "
        "release list %u/4880, search tags %u/4880\n",
        reason,
        U32(s, 0x26328),
        U32(s, 0x30a60) >> 20,
        U32(s, 0x30a70) >> 20,
        static_cast<unsigned>(s[0x30a80]),
        smallFree,
        smallTotal,
        largeFree,
        largeTotal,
        U32(s, 0x729f8),
        U32(s, 0x69174));
    Log("[Blocks] shown lv0/1/2 %u/%u/%u MB, allocated lv0 %u (%u MB) lv1 %u (%u MB) lv2 %u (%u MB), type3 %d (%d MB), "
        "full %u/%u/%u, gc wait %u/%u/%u, small->large %u (%u MB, failed %u)\n",
        U32(s, 0x30a74) >> 20,
        U32(s, 0x30a78) >> 20,
        U32(s, 0x30a7c) >> 20,
        U32(s, 0x3efa0),
        U32(s, 0x3efac) >> 20,
        U32(s, 0x3efa4),
        U32(s, 0x3efb0) >> 20,
        U32(s, 0x3efa8),
        U32(s, 0x3efb4) >> 20,
        static_cast<int32_t>(U32(s, 0x3efc4)),
        static_cast<int32_t>(U32(s, 0x3efc8)) >> 20,
        ts.allocFull[0],
        ts.allocFull[1],
        ts.allocFull[2],
        ts.allocGcWait[0],
        ts.allocGcWait[1],
        ts.allocGcWait[2],
        ts.smallFallbacks,
        static_cast<uint32_t>(ts.smallFallbackBytes >> 20),
        ts.smallFallbackFails);
}

// All three do nothing unless debugLog is set.
void StallMonitor_UpdateBegin(uint8_t* streamer)
{
    if (!GetSettings().debugLog)
        return;
    g_Streamer.store(streamer);
    g_UpdateStart.store(Now());
}

void StallMonitor_UpdateEnd(uint8_t* streamer)
{
    if (!GetSettings().debugLog)
        return;
    const int64_t start = g_UpdateStart.exchange(0);
    const int64_t end = Now();
    if (!start || !streamer)
        return;

    const double ms = Ms(start, end);
    if (ms > kUpdateStallMs)
    {
        char reason[64];
        snprintf(reason, sizeof(reason), "STALL streamer update took %.0f ms", ms);
        LogSnapshot(reason);
    }

    if (!g_LastSnapshot || Ms(g_LastSnapshot, end) > kSnapshotEveryMs)
    {
        g_LastSnapshot = end;
        LogSnapshot("periodic");
    }
}

void StallMonitor_OnPresent()
{
    if (!GetSettings().debugLog)
        return;
    const int64_t now = Now();
    if (g_LastPresent)
    {
        const double gap = Ms(g_LastPresent, now);
        if (gap > kFrameGapMs)
        {
            const int64_t updateStart = g_UpdateStart.load();
            char reason[96];
            if (updateStart)
                snprintf(reason, sizeof(reason), "STALL frame gap %.0f ms, streamer update running for %.0f ms", gap, Ms(updateStart, now));
            else
                snprintf(reason, sizeof(reason), "STALL frame gap %.0f ms, streamer update not running", gap);
            LogSnapshot(reason);
        }
    }
    g_LastPresent = now;
}
