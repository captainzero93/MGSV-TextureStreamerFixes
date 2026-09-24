// DLL entry, hook install thread
#include "pch.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>

#include "MinHook.h"
#include "log.h"
#include "AddressSet.h"
#include "BuiltInModules.h"
#include "DebugConsole.h"
#include "FeatureModule.h"
#include "HookUtils.h"
#include "Settings.h"

bool g_HookBatchMode = false;

// Build marker, checked in the binary by the build script
static const char kBuildMarker[] = "TEXTURESTREAMER,V0_14_20260924";

namespace
{
    static std::atomic_bool gStarted{ false };
}

// Loads settings, resolves addresses, and installs every registered feature module.
static DWORD WINAPI InitThread(LPVOID)
{
    BootTrace("InitThread entered");
    InitLog();
    BootTrace("InitThread log opened");

    Log("[DLL] InitThread started. %s\n", kBuildMarker);

    HMODULE hGame = GetModuleHandleW(nullptr);

    LoadSettings();
    SetBootTrace(GetSettings().debugLog);
    BootTrace("InitThread settings loaded", true);

    const MH_STATUS st = MH_Initialize();
    Log("[DLL] MH_Initialize -> %d\n", static_cast<int>(st));
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
        return 0;

    if (!ResolveAddressSet(hGame))
    {
        Log("[DLL] ResolveAddressSet failed.\n");
        return 0;
    }

    InstallCrashHandler();

    RegisterBuiltInFeatureModules();

    g_HookBatchMode = true;
    const bool allOk = FeatureModuleRegistry::Instance().InstallAll(hGame);
    g_HookBatchMode = false;
    const MH_STATUS applySt = MH_ApplyQueued();
    Log("[DLL] FeatureModuleRegistry::InstallAll -> %s\n", allOk ? "OK" : "PARTIAL/FAIL");
    Log("[DLL] MH_ApplyQueued -> %d\n", static_cast<int>(applySt));

    if (GetSettings().debugWindow)
        StartDebugConsole();

    Log("[DLL] InitThread done.\n");

    BootTrace("InitThread done");
    return 0;
}

// Removes all hooks when the DLL unloads normally.
static void UninstallAll(bool processTerminating)
{
    if (processTerminating)
        return;

    FeatureModuleRegistry::Instance().UninstallAll();

    MH_Uninitialize();
    Log("[DLL] UninstallAll done.\n");

    fflush(stdout);
    fflush(stderr);

    CloseLog();
}

// Standard Windows DLL entry point.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
    {
        BootTrace("DllMain attach", true);
        DisableThreadLibraryCalls(hModule);

        // Pin: Lua frees loadlib handles when its state closes at shutdown, which would
        // unmap the hooks' code while game threads can still be running it.
        HMODULE pinned = nullptr;
        BootTrace(
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(hModule), &pinned)
                ? "DllMain pinned"
                : "DllMain pin FAILED");

        bool expected = false;
        if (!gStarted.compare_exchange_strong(expected, true))
            return TRUE;

        HANDLE hThread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        BootTrace(hThread ? "DllMain CreateThread ok" : "DllMain CreateThread FAILED");
        if (hThread)
            CloseHandle(hThread);

        BootTrace("DllMain return");
        return TRUE;
    }

    case DLL_PROCESS_DETACH:
    {
        UninstallAll(lpReserved != nullptr);
        return TRUE;
    }
    }

    return TRUE;
}
