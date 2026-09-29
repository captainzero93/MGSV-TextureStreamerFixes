// Live texture streamer debug console
#include "pch.h"

#include "DebugConsole.h"
#include "Settings.h"
#include "DeviceVram.h"
#include "TextureStreamerHooks.h"

// Offsets: see RESEARCH.md section 8 (retail 1.0.15.4 EN).
static unsigned int PoolFreeMb(uintptr_t pool, unsigned int* totalMb)
{
    if (!pool)
    {
        *totalMb = 0;
        return 0;
    }
    const unsigned int total = *reinterpret_cast<unsigned int*>(pool + 0x8);
    const unsigned int block = *reinterpret_cast<unsigned int*>(pool + 0xC);
    const unsigned int blocks = *reinterpret_cast<unsigned int*>(pool + 0x18);
    *totalMb = total >> 20;
    const unsigned long long used = static_cast<unsigned long long>(blocks + 1) * block;
    return used >= total ? 0 : static_cast<unsigned int>((total - used) >> 20);
}

static void PrintStreamer(unsigned char* s)
{
    auto ru32 = [&](int off) { return *reinterpret_cast<unsigned int*>(s + off); };
    auto rptr = [&](int off) { return *reinterpret_cast<uintptr_t*>(s + off); };

    printf("[DgTextureStreamer fields] ptr=%p\n", s);

    const uintptr_t tsm = rptr(0x26388);
    if (tsm)
    {
        printf(
            "  config applied: %u MB   pending: %u MB\n",
            *reinterpret_cast<unsigned int*>(tsm + 0x60) >> 20,
            *reinterpret_cast<unsigned int*>(tsm + 0x68) >> 20);
        unsigned int smallTotal = 0;
        unsigned int largeTotal = 0;
        const unsigned int smallFree = PoolFreeMb(*reinterpret_cast<uintptr_t*>(tsm + 0x40), &smallTotal);
        const unsigned int largeFree = PoolFreeMb(*reinterpret_cast<uintptr_t*>(tsm + 0x48), &largeTotal);
        printf("  small pool free: %u / %u MB   large pool free: %u / %u MB\n", smallFree, smallTotal, largeFree, largeTotal);
        // one level 0 block per streamed texture; the streamer tracks at most 4880
        const unsigned int textures = ru32(0x3efa0);
        printf("  streamed textures: %u / 4880  (%.1f%%)\n", textures, textures * 100.0f / 4880.0f);
        const TextureStreamerState& st = GetTextureStreamerState();
        printf("  small->large: %u (failed %u)\n", st.smallFallbacks, st.smallFallbackFails);
    }
    else
    {
        printf("  storage manager not ready\n");
    }

    static const struct
    {
        int off;
        const char* name;
    } kFields[] = {
        { 0x30a54, "vramSize" },     { 0x30a58, "baseReservation" },  { 0x30a5c, "streamingOverhead" },  { 0x30a60, "storageMinusOH" },
        { 0x30a64, "clampedStore" }, { 0x30a68, "baseReservation2" }, { 0x30a6c, "streamingOverhead2" }, { 0x30a70, "textureCacheBudget" },
        { 0x30a74, "shown lv0" },    { 0x30a78, "shown lv1" },        { 0x30a7c, "shown lv2" },
    };

    printf("\n");
    for (const auto& f : kFields)
        printf("  +0x%05x %-20s 0x%08X  (%u MB)\n", f.off, f.name, ru32(f.off), ru32(f.off) >> 20);
    printf("  +0x30a80 degradeFlag          %u\n", ru32(0x30a80));
}

static DWORD WINAPI DebugConsoleThread(LPVOID)
{
    AllocConsole();
    FILE* con = nullptr;
    freopen_s(&con, "CONOUT$", "w", stdout);
    SetConsoleTitleA("MGSV Texture Streamer Debug");

    const Settings& cfg = GetSettings();

    while (true)
    {
        Sleep(500);
        system("cls");

        const TextureStreamerState& st = GetTextureStreamerState();

        printf("=== MGSV Texture Streamer Debug ===\n");
        if (Vram::IsResolved())
            printf("  adapter VRAM:   %llu MB (reported %u MB)\n", Vram::GetBytes() >> 20, Vram::GetReported() >> 20);
        else
            printf("  adapter VRAM:   waiting for the game device\n");
        if (st.requestedSize)
            printf("  requested:      %u MB\n", st.requestedSize >> 20);
        else
            printf("  requested:      not yet\n");
        printf("  patch_dispatch: %s\n", cfg.patchDispatch ? "on" : "off");
        printf("  upgrade/frame:  %d\n", cfg.upgradePerFrame);
        printf("\n");

        if (st.streamer)
            PrintStreamer(static_cast<unsigned char*>(st.streamer));
        else
            printf("[DgTextureStreamer] not yet captured\n");

        printf("\nUpdating every 500ms...\n");
    }
    return 0;
}

void StartDebugConsole()
{
    HANDLE h = CreateThread(nullptr, 0, DebugConsoleThread, nullptr, 0, nullptr);
    if (h)
        CloseHandle(h);
}
