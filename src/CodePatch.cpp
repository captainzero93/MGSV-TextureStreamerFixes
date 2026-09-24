// Checked code byte patches
#include "pch.h"

#include <tlhelp32.h>
#include <vector>

#include "CodePatch.h"
#include "HookUtils.h"
#include "log.h"

static void FormatBytes(const uint8_t* bytes, size_t size, char* out, size_t outSize)
{
    out[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < size && pos + 4 < outSize; ++i)
        pos += snprintf(out + pos, outSize - pos, i ? " %02X" : "%02X", bytes[i]);
}

// Suspends every other thread in the process. Allocates only before the first suspend,
// since a suspended thread may hold the heap lock.
static std::vector<HANDLE> SuspendOtherThreads()
{
    std::vector<DWORD> ids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return {};

    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    THREADENTRY32 te = { sizeof(te) };
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
            ids.push_back(te.th32ThreadID);
    }
    CloseHandle(snap);

    std::vector<HANDLE> threads;
    threads.reserve(ids.size());
    for (DWORD id : ids)
    {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, id);
        if (!h)
            continue;
        if (SuspendThread(h) == static_cast<DWORD>(-1))
        {
            CloseHandle(h);
            continue;
        }
        threads.push_back(h);
    }
    return threads;
}

static void ResumeThreads(std::vector<HANDLE>& threads)
{
    for (HANDLE h : threads)
    {
        ResumeThread(h);
        CloseHandle(h);
    }
    threads.clear();
}

// A suspended thread with RIP inside a site would resume mid-instruction.
static bool AnyThreadInside(const std::vector<HANDLE>& threads, const uintptr_t* addrs, const CodePatch* patches, size_t count)
{
    for (HANDLE h : threads)
    {
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(h, &ctx))
            continue;
        for (size_t i = 0; i < count; ++i)
        {
            if (ctx.Rip > addrs[i] && ctx.Rip < addrs[i] + patches[i].size)
                return true;
        }
    }
    return false;
}

static bool WriteCode(void* addr, const uint8_t* data, size_t size)
{
    DWORD old;
    if (!VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(addr, data, size);
    VirtualProtect(addr, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, size);
    return true;
}

bool ApplyPatchGroup(const char* groupName, const CodePatch* patches, size_t count)
{
    char cur[64];
    char exp[64];
    bool ok = true;

    for (size_t i = 0; i < count; ++i)
    {
        const CodePatch& p = patches[i];
        const uint8_t* addr = static_cast<const uint8_t*>(ResolveGameAddress(p.absAddr));
        if (!addr)
        {
            Log("[Patch] %s: %s unresolved\n", groupName, p.name);
            ok = false;
            continue;
        }

        FormatBytes(addr, p.size, cur, sizeof(cur));
        if (!p.expected)
        {
            Log("[Patch] %s: %s @0x%llX no expected bytes, orig [%s]\n", groupName, p.name, (unsigned long long)p.absAddr, cur);
            ok = false;
        }
        else if (memcmp(addr, p.expected, p.size) != 0)
        {
            FormatBytes(p.expected, p.size, exp, sizeof(exp));
            Log("[Patch] %s: %s @0x%llX MISMATCH orig [%s] expected [%s]\n", groupName, p.name, (unsigned long long)p.absAddr, cur, exp);
            ok = false;
        }
        else
        {
            Log("[Patch] %s: %s @0x%llX orig [%s] matches\n", groupName, p.name, (unsigned long long)p.absAddr, cur);
        }
    }

    if (!ok)
    {
        Log("[Patch] %s: SKIPPED, nothing written\n", groupName);
        return false;
    }

    // resolve before suspending, GetModuleHandleW takes a loader lock a suspended thread may hold
    std::vector<uintptr_t> addrs(count);
    for (size_t i = 0; i < count; ++i)
        addrs[i] = reinterpret_cast<uintptr_t>(ResolveGameAddress(patches[i].absAddr));

    std::vector<HANDLE> threads = SuspendOtherThreads();
    if (AnyThreadInside(threads, addrs.data(), patches, count))
    {
        ResumeThreads(threads);
        Log("[Patch] %s: SKIPPED, a thread was inside a patch site\n", groupName);
        return false;
    }

    bool written = true;
    for (size_t i = 0; i < count && written; ++i)
        written = WriteCode(reinterpret_cast<void*>(addrs[i]), patches[i].patch, patches[i].size);
    ResumeThreads(threads);

    Log("[Patch] %s: %s\n", groupName, written ? "applied" : "VirtualProtect failed, group partially applied");
    return written;
}

bool CheckTargetBytes(const char* name, uintptr_t absAddr, const uint8_t* expected, size_t size)
{
    const uint8_t* addr = static_cast<const uint8_t*>(ResolveGameAddress(absAddr));
    if (!addr)
        return false;
    char cur[64];
    FormatBytes(addr, 16, cur, sizeof(cur));
    if (!expected)
    {
        Log("[Hook] %s @0x%llX bytes [%s], no expected bytes, skipped\n", name, (unsigned long long)absAddr, cur);
        return false;
    }
    const bool match = memcmp(addr, expected, size) == 0;
    Log("[Hook] %s @0x%llX bytes [%s] %s\n", name, (unsigned long long)absAddr, cur, match ? "matches" : "MISMATCH, skipped");
    return match;
}
