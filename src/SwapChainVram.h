// VRAM from the game's swapchain device
#pragma once

#include <cstdint>

bool Install_SwapChainVram_Hook();
bool Uninstall_SwapChainVram_Hook();

namespace Vram
{
    bool IsResolved();
    uint64_t GetBytes();    // adapter VRAM, 0 until resolved
    uint32_t GetReported(); // value given to the game, 0 = leave the game's own
}
