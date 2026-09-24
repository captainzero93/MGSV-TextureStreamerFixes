// VRAM from the game's swapchain device
#include "pch.h"

#include <atomic>
#include <dxgi1_4.h>

#include "HookUtils.h"
#include "PatternScan.h"
#include "Settings.h"
#include "StallMonitor.h"
#include "SwapChainVram.h"
#include "log.h"

namespace
{
    // gn::swapchain::Present, the game's wrapper around IDXGISwapChain::Present.
    // Pattern from IHHook (0x-FADED, commit 0e282a0, 21/09/2026, D3D11Hook.cpp).
    // VERIFIED in Ghidra, retail 1.0.15.4 EN: pattern at 0x14024CEFC calls 0x1419F5990, which does
    // MOV RCX,[RCX+0x18]; XOR R8D,R8D; JMP [vtable+0x40], so arg+0x18 is the IDXGISwapChain.
    constexpr const char* kPresentPattern = "E8 ? ? ? ? 85 C0 75 12 38 43 69";
    constexpr size_t kSwapChainOffset = 0x18;

    using Present_t = HRESULT(__fastcall*)(uintptr_t self, int64_t syncInterval);

    Present_t g_OrigPresent = nullptr;
    void* g_PresentTarget = nullptr;
    std::atomic<bool> g_Attempted{ false };
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
    BootTrace("Vram GetDesc");
    adapter->GetDesc(&desc);
    const uint64_t dedicated = desc.DedicatedVideoMemory;

    uint64_t budget = 0;
    IDXGIAdapter3* adapter3 = nullptr;
    BootTrace("Vram QueryInterface IDXGIAdapter3");
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

// swapchain -> IDXGIDevice -> GetAdapter, no CreateDXGIFactory1
static void ResolveFromSwapChain(IDXGISwapChain* swapChain)
{
    IDXGIDevice* dxgiDevice = nullptr;
    BootTrace("Vram GetDevice");
    if (FAILED(swapChain->GetDevice(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))))
    {
        Log("[Vram] GetDevice failed\n");
        return;
    }

    IDXGIAdapter* adapter = nullptr;
    BootTrace("Vram GetAdapter");
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
    {
        g_VramBytes.store(QueryAdapterVram(adapter));
        adapter->Release();
    }
    dxgiDevice->Release();

    if (g_VramBytes.load())
        Log("[Vram] Resolved on first Present: %llu MB, reported to game %u MB\n", g_VramBytes.load() >> 20, Vram::GetReported() >> 20);
    else
        Log("[Vram] ERROR: could not read VRAM from game adapter\n");
}

static HRESULT __fastcall hkPresent(uintptr_t self, int64_t syncInterval)
{
    if (!g_Attempted.exchange(true))
    {
        BootTrace("hkPresent first call");
        IDXGISwapChain* swapChain = self ? *reinterpret_cast<IDXGISwapChain**>(self + kSwapChainOffset) : nullptr;
        if (swapChain)
            ResolveFromSwapChain(swapChain);
        else
            Log("[Vram] ERROR: null swapchain at Present arg+0x%zX\n", kSwapChainOffset);
        BootTrace("hkPresent calling original");
        const HRESULT hr = g_OrigPresent(self, syncInterval);
        BootTrace("hkPresent original returned");
        return hr;
    }
    StallMonitor_OnPresent();
    return g_OrigPresent(self, syncInterval);
}

bool Install_SwapChainVram_Hook()
{
    if (!GetSettings().hookPresent)
    {
        Log("[Vram] hookPresent=false, Present hook not installed\n");
        return true;
    }

    size_t matches = 0;
    uint8_t* call = FindGamePattern(kPresentPattern, &matches);
    if (!call || matches != 1)
    {
        Log("[Vram] Present pattern matches=%zu, expected 1. Hook not installed\n", matches);
        return false;
    }

    uint8_t* target = ResolveRel32(call + 1);
    // older exes route through a jmp thunk; only follow thunks inside the exe,
    // not another hook's relay
    if (*target == 0xE9 && IsInGameImage(ResolveRel32(target + 1)))
        target = ResolveRel32(target + 1);

    const uintptr_t absTarget = reinterpret_cast<uintptr_t>(target) - GetExeBase() + EXE_PREFERRED_BASE;
    Log("[Vram] Present pattern @0x%llX -> gn::swapchain::Present 0x%llX\n",
        (unsigned long long)(reinterpret_cast<uintptr_t>(call) - GetExeBase() + EXE_PREFERRED_BASE),
        (unsigned long long)absTarget);

    char bytes[64] = {};
    for (int i = 0, pos = 0; i < 16; ++i)
        pos += snprintf(bytes + pos, sizeof(bytes) - pos, i ? " %02X" : "%02X", target[i]);
    Log("[Vram] gn::swapchain::Present bytes [%s]\n", bytes);

    const bool ok = CreateAndEnableHook(target, reinterpret_cast<void*>(&hkPresent), reinterpret_cast<void**>(&g_OrigPresent));
    if (ok)
        g_PresentTarget = target;
    Log("[Hook] Present: %s\n", ok ? "OK" : "FAIL");
    return ok;
}

bool Uninstall_SwapChainVram_Hook()
{
    if (g_PresentTarget)
        DisableAndRemoveHook(g_PresentTarget);
    g_PresentTarget = nullptr;
    g_OrigPresent = nullptr;
    return true;
}
