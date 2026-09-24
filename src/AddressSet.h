// Per-build game addresses
#pragma once

#include <Windows.h>
#include <cstdint>

namespace AddressSetRuntime
{
    enum class GameBuild
    {
        Unknown,
        En_1_0_15_3, // day1820
        Jp_1_0_15_3, // day1820
        En_1_0_15_4, // day3900mgo, SHA256 085c2f82...bb45
        Jp_1_0_15_4  // day3800
    };

    // Field order must match kAddrFieldNames in AddressSet.cpp.
    struct AddressSet
    {
        // hooks
        uintptr_t StreamerUpdate = 0;
        uintptr_t GetAvailableStorageMemorySize = 0;
        uintptr_t RequestTextureStorageConfiguration = 0;
        uintptr_t TsmAllocBlock = 0;

        // patch sites in StreamerUpdate and doListupChangegraders
        uintptr_t UpdateAvailCap = 0;
        uintptr_t UpdateDegradeThreshold = 0;
        uintptr_t UpdateForcedDegrade = 0;
        uintptr_t ListupChangegradersUpgradeCap = 0;
    };

    inline GameBuild& GetGameBuild()
    {
        static GameBuild value = GameBuild::Unknown;
        return value;
    }

    inline AddressSet& GetAddressSet()
    {
        static AddressSet value{};
        return value;
    }

    GameBuild DetectGameBuild(HMODULE hGame); // by exe SHA256
    bool ResolveAddressSet(HMODULE hGame);
    bool HasAllAddresses();
    void InstallCrashHandler();

    inline const char* GetGameBuildName(GameBuild build)
    {
        switch (build)
        {
        case GameBuild::En_1_0_15_3:
            return "EN 1.0.15.3";
        case GameBuild::Jp_1_0_15_3:
            return "JP 1.0.15.3";
        case GameBuild::En_1_0_15_4:
            return "EN 1.0.15.4";
        case GameBuild::Jp_1_0_15_4:
            return "JP 1.0.15.4";
        default:
            return "Unknown";
        }
    }
}

#define gGameBuild (::AddressSetRuntime::GetGameBuild())
#define gAddr (::AddressSetRuntime::GetAddressSet())
#define ResolveAddressSet (::AddressSetRuntime::ResolveAddressSet)
#define InstallCrashHandler (::AddressSetRuntime::InstallCrashHandler)
#define GetGameBuildName (::AddressSetRuntime::GetGameBuildName)
