// Code pattern scan over the game exe
#include "pch.h"

#include <vector>

#include "PatternScan.h"

static bool GetGameImage(uintptr_t& base, size_t& size, const IMAGE_NT_HEADERS** ntOut)
{
    HMODULE h = GetModuleHandleW(nullptr);
    if (!h)
        return false;
    base = reinterpret_cast<uintptr_t>(h);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    size = nt->OptionalHeader.SizeOfImage;
    if (ntOut)
        *ntOut = nt;
    return true;
}

bool IsInGameImage(const void* p)
{
    uintptr_t base = 0;
    size_t size = 0;
    if (!GetGameImage(base, size, nullptr))
        return false;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return a >= base && a < base + size;
}

uint8_t* ResolveRel32(uint8_t* p)
{
    int32_t rel;
    memcpy(&rel, p, sizeof(rel));
    return p + 4 + rel;
}

uint8_t* FindGamePattern(const char* pattern, size_t* matchCount)
{
    std::vector<int> bytes; // -1 = wildcard
    for (const char* c = pattern; *c;)
    {
        if (*c == ' ')
        {
            ++c;
            continue;
        }
        if (*c == '?')
        {
            bytes.push_back(-1);
            while (*c == '?')
                ++c;
            continue;
        }
        bytes.push_back(static_cast<int>(strtoul(c, const_cast<char**>(&c), 16)));
    }

    uintptr_t base = 0;
    size_t imageSize = 0;
    const IMAGE_NT_HEADERS* nt = nullptr;
    size_t count = 0;
    uint8_t* first = nullptr;

    if (!bytes.empty() && GetGameImage(base, imageSize, &nt))
    {
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec)
        {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
                continue;
            uint8_t* start = reinterpret_cast<uint8_t*>(base + sec->VirtualAddress);
            const size_t len = sec->Misc.VirtualSize;
            if (len < bytes.size())
                continue;
            for (size_t i = 0; i <= len - bytes.size(); ++i)
            {
                size_t j = 0;
                while (j < bytes.size() && (bytes[j] < 0 || start[i + j] == bytes[j]))
                    ++j;
                if (j == bytes.size())
                {
                    if (!first)
                        first = start + i;
                    ++count;
                }
            }
        }
    }

    if (matchCount)
        *matchCount = count;
    return first;
}
