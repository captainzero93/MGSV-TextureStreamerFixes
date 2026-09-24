// Feature module registration
#include "pch.h"

#include <Windows.h>
#include <mutex>

#include "BuiltInModules.h"
#include "FeatureModule.h"
#include "SwapChainVram.h"
#include "TextureStreamerHooks.h"

namespace
{
    class SwapChainVramModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "SwapChainVram";
        }

        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_SwapChainVram_Hook();
        }

        void Uninstall() override
        {
            Uninstall_SwapChainVram_Hook();
        }
    };

    class TextureStreamerModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "TextureStreamer";
        }

        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_TextureStreamer_Hooks();
        }

        void Uninstall() override
        {
            Uninstall_TextureStreamer_Hooks();
        }
    };
}

void RegisterBuiltInFeatureModules()
{
    static SwapChainVramModule s_SwapChainVramModule;
    static TextureStreamerModule s_TextureStreamerModule;

    static std::once_flag s_Once;
    std::call_once(
        s_Once,
        []()
        {
            FeatureModuleRegistry::Instance().Register(&s_SwapChainVramModule);
            FeatureModuleRegistry::Instance().Register(&s_TextureStreamerModule);
        });
}
