// Feature module registration
#include "pch.h"

#include <Windows.h>
#include <mutex>

#include "BuiltInModules.h"
#include "FeatureModule.h"
#include "DeviceVram.h"
#include "TextureStreamerHooks.h"

namespace
{
    class DeviceVramModule final : public IFeatureModule
    {
    public:
        const char* GetName() const override
        {
            return "DeviceVram";
        }

        bool Install(HMODULE hGame) override
        {
            UNREFERENCED_PARAMETER(hGame);
            return Install_DeviceVram();
        }

        void Uninstall() override
        {
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
    static DeviceVramModule s_DeviceVramModule;
    static TextureStreamerModule s_TextureStreamerModule;

    static std::once_flag s_Once;
    std::call_once(
        s_Once,
        []()
        {
            FeatureModuleRegistry::Instance().Register(&s_DeviceVramModule);
            FeatureModuleRegistry::Instance().Register(&s_TextureStreamerModule);
        });
}
