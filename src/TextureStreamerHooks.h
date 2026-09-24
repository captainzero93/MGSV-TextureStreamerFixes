// Texture streamer budget hooks and patches
#pragma once

#include <cstdint>

bool Install_TextureStreamer_Hooks();
bool Uninstall_TextureStreamer_Hooks();

struct TextureStreamerState
{
    void* streamer;              // DgTextureStreamer, from the per-frame update
    uint32_t requestedSize;      // budget requested from VRAM, 0 until sent
    uint32_t smallFallbacks;     // small pool allocations placed in the large pool
    uint32_t smallFallbackFails; // small pool full and the large pool retry failed too
    uint64_t smallFallbackBytes; // total requested by those, never reduced on free
    uint32_t allocFull[3];       // AllocBlock 0xFFFFFFFF per slot, game's own result before any retry
    uint32_t allocGcWait[3];     // AllocBlock 0xFFFFFFFE per slot (fragmented, compaction pending)
};

const TextureStreamerState& GetTextureStreamerState();
