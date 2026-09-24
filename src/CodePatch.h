// Checked code byte patches
#pragma once

#include <cstddef>
#include <cstdint>

struct CodePatch
{
    uintptr_t absAddr;       // preferred-base address, rebased at apply time
    const uint8_t* expected; // original bytes, required
    const uint8_t* patch;
    size_t size;
    const char* name;
};

// Applies all patches or none, with other threads suspended. Nothing is written if any
// site lacks expected bytes, does not hold them, or a suspended thread is inside a site.
bool ApplyPatchGroup(const char* groupName, const CodePatch* patches, size_t count);

// True if the bytes at absAddr equal expected. Logs the bytes either way.
bool CheckTargetBytes(const char* name, uintptr_t absAddr, const uint8_t* expected, size_t size);
