// VRAM from the game's own D3D11 device
#include "pch.h"

#include <atomic>
#include <d3d11.h>
#include <dxgi1_4.h>

#include "DeviceVram.h"
#include "HookUtils.h"
#include "PatternScan.h"
#include "Settings.h"
#include "log.h"

namespace
{
    // gn device init FUN_1419f4eb0 (retail 1.0.15.4 EN) stores D3D11CreateDevice's device at 0x142C6B870 and checks it:
    // 1419f4f29  TEST EAX,EAX; JS; MOV RCX,[device]; TEST RCX,RCX; JZ; CMP qword [context],0; JZ
    constexpr const char* kDevicePattern = "85 C0 0F 88 ? ? ? ? 48 8B 0D ? ? ? ? 48 85 C9 0F 84 ? ? ? ? 48 83 3D ? ? ? ? 00 0F 84";
    constexpr size_t kMovRcxOffset = 8; // 48 8B 0D rel32, 7 bytes

    ID3D11Device** g_DeviceGlobal = nullptr;
    std::atomic<bool> g_Failed{ false };
    std::atomic<uint64_t> g_VramBytes{ 0 };
}

bool Vram::IsResolved()
{
    return g_VramBytes.load() != 0;
}

uint64_t Vram::GetBytes()
{
    return g_VramBytes.load();
}

uint32_t Vram::GetReported()
{
    if (GetSettings().lockVramMax)
        return 0xFFFFFFFFu;
    const uint64_t bytes = g_VramBytes.load();
    return bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes);
}

// Larger of dedicated (physical) and IDXGIAdapter3 budget (runtime).
static uint64_t QueryAdapterVram(IDXGIAdapter* adapter)
{
    DXGI_ADAPTER_DESC desc = {};
    adapter->GetDesc(&desc);
    const uint64_t dedicated = desc.DedicatedVideoMemory;

    uint64_t budget = 0;
    IDXGIAdapter3* adapter3 = nullptr;
    if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))))
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            budget = info.Budget;
        Log("[Vram] Budget=%llu CurrentUsage=%llu\n", info.Budget, info.CurrentUsage);
        adapter3->Release();
    }
    else
    {
        Log("[Vram] IDXGIAdapter3 not available, using dedicated only\n");
    }

    const uint64_t result = budget > dedicated ? budget : dedicated;
    Log("[Vram] Adapter '%ls' dedicated=%llu MB budget=%llu MB using=%llu MB\n", desc.Description, dedicated >> 20, budget >> 20, result >> 20);
    return result;
}

// device -> IDXGIDevice -> GetAdapter. Returns true once resolved or given up.
bool Vram::TryResolve()
{
    if (g_VramBytes.load() || g_Failed.load())
        return true;
    if (!g_DeviceGlobal)
        return true;

    ID3D11Device* device = *g_DeviceGlobal;
    if (!device)
        return false; // not created yet, retried from the streamer update

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))))
    {
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
        {
            g_VramBytes.store(QueryAdapterVram(adapter));
            adapter->Release();
        }
        dxgiDevice->Release();
    }

    if (g_VramBytes.load())
        Log("[Vram] Resolved: %llu MB, reported to game %u MB\n", g_VramBytes.load() >> 20, Vram::GetReported() >> 20);
    else
    {
        g_Failed.store(true);
        Log("[Vram] ERROR: could not read VRAM from the game's device\n");
    }
    return true;
}

bool Install_DeviceVram()
{
    size_t matches = 0;
    uint8_t* match = FindGamePattern(kDevicePattern, &matches);
    if (!match || matches != 1)
    {
        Log("[Vram] Device pattern matches=%zu, expected 1. Game budget left alone\n", matches);
        return false;
    }

    g_DeviceGlobal = reinterpret_cast<ID3D11Device**>(ResolveRel32(match + kMovRcxOffset + 3));
    Log("[Vram] Device pattern @0x%llX -> device global 0x%llX\n",
        (unsigned long long)(reinterpret_cast<uintptr_t>(match) - GetExeBase() + EXE_PREFERRED_BASE),
        (unsigned long long)(reinterpret_cast<uintptr_t>(g_DeviceGlobal) - GetExeBase() + EXE_PREFERRED_BASE));

    if (!Vram::TryResolve() || !Vram::IsResolved())
        Log("[Vram] Device not created yet, retrying from the streamer update\n");
    return true;
}
