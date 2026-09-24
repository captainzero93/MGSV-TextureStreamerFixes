// Per-build game addresses, exe hash detection, crash logger
#include "pch.h"

#include <Windows.h>
#include <bcrypt.h>
#include <cstring>

#include "AddressSet.h"
#include "log.h"

#pragma comment(lib, "bcrypt.lib")

static const char* const kAddrFieldNames[] = {
    "StreamerUpdate",         "GetAvailableStorageMemorySize", "RequestTextureStorageConfiguration", "TsmAllocBlock", "UpdateAvailCap",
    "UpdateDegradeThreshold", "UpdateForcedDegrade",           "ListupChangegradersUpgradeCap",
};
static const int kAddrFieldCount = sizeof(kAddrFieldNames) / sizeof(kAddrFieldNames[0]);
static_assert(sizeof(AddressSetRuntime::AddressSet) == sizeof(kAddrFieldNames) / sizeof(kAddrFieldNames[0]) * sizeof(uintptr_t), "kAddrFieldNames out of sync");

namespace AddressSetRuntime
{
    namespace
    {
        // mgsvtpp.exe 1.0.15.4 EN, version_info.txt "tpp_steam_mst_en_day3900mgo_patch_0707_1632"
        constexpr const char* kSha256_1_0_15_4_En = "085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45";

        // SHA256 of the exe file on disk, lowercase hex. Empty on failure.
        std::string ExeSha256(HMODULE hGame)
        {
            wchar_t path[MAX_PATH] = {};
            if (!GetModuleFileNameW(hGame, path, MAX_PATH))
                return {};

            HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return {};

            std::string hex;
            BCRYPT_ALG_HANDLE alg = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
                && BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)))
            {
                static unsigned char buf[1 << 20];
                DWORD read = 0;
                bool ok = true;
                while (ok && ReadFile(file, buf, sizeof(buf), &read, nullptr) && read)
                    ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf, read, 0));

                unsigned char digest[32];
                if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0)))
                {
                    char h[65];
                    for (int i = 0; i < 32; ++i)
                        snprintf(h + i * 2, 3, "%02x", digest[i]);
                    hex = h;
                }
            }
            if (hash)
                BCryptDestroyHash(hash);
            if (alg)
                BCryptCloseAlgorithmProvider(alg, 0);
            CloseHandle(file);
            return hex;
        }
    }

    // VERIFIED in Ghidra (project MGSV, program mgsvtpp.exe, retail 1.0.15.4 EN) on 24/09/2026.
    // Names in quotes are from the previous mgsv_mod source (inferred); the old addresses were
    // from another build, offset +0x4D0 (+0x4E0 in the update function) from these.
    const AddressSet& Get_mst_en_day3900_AddressSet() // 1.0.15.4 english
    {
        static const AddressSet value = {
            0x14021A460ull, // retail 1.0.15.4 EN, FUN_14021a460, streamer per-frame update ("dispatch")
            0x1402A8F60ull, // retail 1.0.15.4 EN, FUN_1402a8f60, "GetAvailableStorageMemorySize"
            0x1402A91D0ull, // retail 1.0.15.4 EN, FUN_1402a91d0, "RequestTextureStorageConfiguration"
            0x1402A6D40ull, // retail 1.0.15.4 EN, FUN_1402a6d40, TextureStorageManager::AllocBlock (ORs 0xAC910000)
            0x14021A772ull, // retail 1.0.15.4 EN, FUN_14021a460+0x312, MOV EBX,0xE0000000 (3584MB cap)
            0x14021A797ull, // retail 1.0.15.4 EN, FUN_14021a460+0x337, CMOVC R15D,EAX (degrade under 800MB)
            0x14021A7A0ull, // retail 1.0.15.4 EN, FUN_14021a460+0x340, JZ (degrade when config flag set)
            0x1402216E0ull, // retail 1.0.15.4 EN, FUN_1402216b0+0x30, CMP [RBX+0xB8],0x10 ("doListupChangegraders")
        };
        return value;
    }

    GameBuild DetectGameBuild(HMODULE hGame)
    {
        const std::string sha = ExeSha256(hGame);
        Log("[AddressSet] mgsvtpp.exe SHA256 %s\n", sha.empty() ? "<failed>" : sha.c_str());
        if (sha == kSha256_1_0_15_4_En)
            return GameBuild::En_1_0_15_4;
        return GameBuild::Unknown;
    }

    bool ResolveAddressSet(HMODULE hGame)
    {
        if (!hGame)
            return false;

        GetGameBuild() = DetectGameBuild(hGame);

        switch (GetGameBuild())
        {
        case GameBuild::En_1_0_15_4:
            GetAddressSet() = Get_mst_en_day3900_AddressSet();
            break;
        default:
            GetAddressSet() = AddressSet{}; // unknown exe, install nothing
            break;
        }
        Log("[AddressSet] Selected %s address set.\n", GetGameBuildName(GetGameBuild()));
        return true;
    }

    bool HasAllAddresses()
    {
        const uintptr_t* vals = reinterpret_cast<const uintptr_t*>(&GetAddressSet());
        for (int i = 0; i < kAddrFieldCount; ++i)
        {
            if (!vals[i])
            {
                Log("[AddressSet] Missing %s for %s.\n", kAddrFieldNames[i], GetGameBuildName(GetGameBuild()));
                return false;
            }
        }
        return true;
    }

    namespace
    {
        LPTOP_LEVEL_EXCEPTION_FILTER g_PrevCrashFilter = nullptr;

        void GetGameModuleRange(uintptr_t& base, uintptr_t& size)
        {
            base = 0;
            size = 0;
            HMODULE h = GetModuleHandleW(nullptr);
            if (!h)
                return;
            base = reinterpret_cast<uintptr_t>(h);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(h) + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return;
            size = nt->OptionalHeader.SizeOfImage;
        }

        const char* ModuleNameFromAddr(const void* addr, uintptr_t& base)
        {
            base = 0;
            HMODULE hm = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addr), &hm)
                && hm)
            {
                base = reinterpret_cast<uintptr_t>(hm);
                static char name[MAX_PATH];
                if (GetModuleFileNameA(hm, name, MAX_PATH))
                {
                    const char* slash = strrchr(name, '\\');
                    return slash ? slash + 1 : name;
                }
            }
            return "<unknown-module>";
        }

        LONG WINAPI TextureStreamerCrashFilter(EXCEPTION_POINTERS* ep)
        {
            __try
            {
                const EXCEPTION_RECORD* er = ep->ExceptionRecord;
                const CONTEXT* cx = ep->ContextRecord;
                const uintptr_t fault = reinterpret_cast<uintptr_t>(er->ExceptionAddress);

                uintptr_t gameBase = 0, gameSize = 0;
                GetGameModuleRange(gameBase, gameSize);

                uintptr_t modBase = 0;
                const char* modName = ModuleNameFromAddr(er->ExceptionAddress, modBase);

                CrashLogf("\n==================== TextureStreamer CRASH ====================\n");
                CrashLogf("[CRASH] build=%s  exceptionCode=0x%08lX\n", GetGameBuildName(GetGameBuild()), static_cast<unsigned long>(er->ExceptionCode));
                CrashLogf(
                    "[CRASH] faulting instruction @ 0x%llX  (%s + 0x%llX)\n",
                    static_cast<unsigned long long>(fault),
                    modName,
                    static_cast<unsigned long long>(modBase ? fault - modBase : 0ull));

                if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
                {
                    const ULONG_PTR kind = er->ExceptionInformation[0];
                    const char* op = (kind == 0) ? "READ" : (kind == 1) ? "WRITE" : (kind == 8) ? "EXECUTE" : "ACCESS";
                    CrashLogf("[CRASH] access violation: tried to %s 0x%llX\n", op, static_cast<unsigned long long>(er->ExceptionInformation[1]));
                }

                const uintptr_t* vals = reinterpret_cast<const uintptr_t*>(&GetAddressSet());
                const int n = static_cast<int>(sizeof(AddressSet) / sizeof(uintptr_t));
                const char* bestName = nullptr;
                uintptr_t bestAddr = 0;
                for (int i = 0; i < n && i < kAddrFieldCount; ++i)
                {
                    const uintptr_t a = vals[i];
                    if (a && a <= fault && a > bestAddr)
                    {
                        bestAddr = a;
                        bestName = kAddrFieldNames[i];
                    }
                }
                if (bestName)
                    CrashLogf(
                        "[CRASH] nearest hooked address at/below the fault: %s @ 0x%llX  (fault = %s + 0x%llX)\n",
                        bestName,
                        static_cast<unsigned long long>(bestAddr),
                        bestName,
                        static_cast<unsigned long long>(fault - bestAddr));
                else
                    CrashLogf("[CRASH] the fault is below every resolved hooked address.\n");

                CrashLogf(
                    "[CRASH] RIP=%016llX RSP=%016llX RBP=%016llX\n", (unsigned long long)cx->Rip, (unsigned long long)cx->Rsp, (unsigned long long)cx->Rbp);
                CrashLogf(
                    "[CRASH] RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX\n",
                    (unsigned long long)cx->Rax,
                    (unsigned long long)cx->Rbx,
                    (unsigned long long)cx->Rcx,
                    (unsigned long long)cx->Rdx);
                CrashLogf(
                    "[CRASH] RSI=%016llX RDI=%016llX R8 =%016llX R9 =%016llX\n",
                    (unsigned long long)cx->Rsi,
                    (unsigned long long)cx->Rdi,
                    (unsigned long long)cx->R8,
                    (unsigned long long)cx->R9);
                CrashLogf(
                    "[CRASH] R10=%016llX R11=%016llX R12=%016llX R13=%016llX\n",
                    (unsigned long long)cx->R10,
                    (unsigned long long)cx->R11,
                    (unsigned long long)cx->R12,
                    (unsigned long long)cx->R13);
                CrashLogf("[CRASH] R14=%016llX R15=%016llX\n", (unsigned long long)cx->R14, (unsigned long long)cx->R15);

                CrashLogf("[CRASH] stack return-address trail (game-module addrs == disassembly-dump addrs, no ASLR):\n");
                if (gameSize)
                {
                    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(cx->Rsp);
                    int shown = 0;
                    for (int i = 0; i < 256 && shown < 12; ++i)
                    {
                        const uintptr_t v = sp[i];
                        if (v >= gameBase && v < gameBase + gameSize)
                        {
                            CrashLogf("[CRASH]   [rsp+0x%03X] 0x%llX  (game+0x%llX)\n", i * 8, (unsigned long long)v, (unsigned long long)(v - gameBase));
                            ++shown;
                        }
                    }
                    if (shown == 0)
                        CrashLogf("[CRASH]   (no game-module return addresses in the first 2KB of stack)\n");
                }
                CrashLogf("[CRASH] Look up the faulting address / game+offset in this build's disassembly dump.\n");
                CrashLogf("===========================================================\n");
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                CrashLogf("[CRASH] (the crash logger itself faulted while writing the report)\n");
            }

            return g_PrevCrashFilter ? g_PrevCrashFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
        }
    }

    void InstallCrashHandler()
    {
        g_PrevCrashFilter = SetUnhandledExceptionFilter(TextureStreamerCrashFilter);
        Log("[CRASH] Unhandled-exception crash logger installed.\n");
    }
}
