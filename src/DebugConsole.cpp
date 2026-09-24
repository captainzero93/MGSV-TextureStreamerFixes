// Live texture streamer debug console
#include "pch.h"

#include "DebugConsole.h"
#include "Settings.h"
#include "SwapChainVram.h"
#include "TextureStreamerHooks.h"

// DgTextureStreamer +0x30a54..0x30a70, +0x30a80 and +0x26388: VERIFIED in FUN_14021dee0 and FUN_14021a460 (retail 1.0.15.4 EN).
// +0x30a74..0x30a7c, TSM +0x28/+0x40/+0x48 and the +0x18 counts: from the previous mgsv_mod source, UNVERIFIED
static void PrintStreamer(unsigned char* s)
{
    auto ru32 = [&](int off) { return *reinterpret_cast<unsigned int*>(s + off); };
    auto rptr = [&](int off) { return *reinterpret_cast<uintptr_t*>(s + off); };

    printf("[DgTextureStreamer fields] ptr=%p\n", s);

    const uintptr_t tsm = rptr(0x26388);
    if (tsm)
    {
        // retail 1.0.15.4 EN: TSM+0x60 applied config, +0x68 pending (FUN_1402a91d0)
        printf(
            "  config applied: %u MB   pending: %u MB\n",
            *reinterpret_cast<unsigned int*>(tsm + 0x60) >> 20,
            *reinterpret_cast<unsigned int*>(tsm + 0x68) >> 20);
        const unsigned int cap = *reinterpret_cast<unsigned int*>(tsm + 0x28);
        const uintptr_t ba0 = *reinterpret_cast<uintptr_t*>(tsm + 0x40);
        const uintptr_t ba1 = *reinterpret_cast<uintptr_t*>(tsm + 0x48);
        const unsigned int used0 = ba0 ? *reinterpret_cast<unsigned int*>(ba0 + 0x18) : 0;
        const unsigned int used1 = ba1 ? *reinterpret_cast<unsigned int*>(ba1 + 0x18) : 0;
        const unsigned int total = used0 + used1;
        printf("  handles in use:        %u / %u  (%.1f%%)\n", total, cap, cap ? (total * 100.0f / cap) : 0.f);
        printf("    type0: %u   type1: %u\n", used0, used1);
    }
    else
    {
        printf("  handles: TSM not ready\n");
    }

    static const struct
    {
        int off;
        const char* name;
    } kFields[] = {
        { 0x30a54, "vramSize" },     { 0x30a58, "baseReservation" },  { 0x30a5c, "streamingOverhead" },  { 0x30a60, "storageMinusOH" },
        { 0x30a64, "clampedStore" }, { 0x30a68, "baseReservation2" }, { 0x30a6c, "streamingOverhead2" }, { 0x30a70, "textureCacheBudget" },
        { 0x30a74, "type0 used" },   { 0x30a78, "type1 used" },       { 0x30a7c, "type2 used" },
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
            printf("  adapter VRAM:   waiting for first Present\n");
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
