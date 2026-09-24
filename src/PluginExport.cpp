// luaopen export for package.loadlib
#include "pch.h"

#include "log.h"

struct lua_State;

// Hooks install from DllMain's InitThread; this only has to exist for loadlib.
extern "C" __declspec(dllexport) int __cdecl luaopen_TextureStreamer(lua_State* L)
{
    UNREFERENCED_PARAMETER(L);
    BootTrace("luaopen enter");
    Log("[DLL] luaopen_TextureStreamer called\n");
    BootTrace("luaopen return");
    return 0;
}
