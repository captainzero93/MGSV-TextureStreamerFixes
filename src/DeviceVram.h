// VRAM from the game's own D3D11 device
#pragma once

#include <cstdint>

bool Install_DeviceVram();

namespace Vram
{
    bool TryResolve(); // reads the device global; true once VRAM is known
    bool IsResolved();
    uint64_t GetBytes();    // adapter VRAM, 0 until resolved
    uint32_t GetReported(); // value given to the game, 0 = leave the game's own
}
