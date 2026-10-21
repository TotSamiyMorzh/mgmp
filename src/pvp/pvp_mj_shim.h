// pvp_mj_shim.h -- the slice of the Mewjector API the arena code was written
// against (pvp/probe used Mewjector's version.dll loader), served by mgmp's own
// MinHook instead. Inside mgmp.dll there is no second loader: the arena's hooks
// are installed by pvp_init(), after mgmp's own hooks_install() has initialised
// MinHook, and before the loader unparks the game.
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Detour base+rva -> fn; *tramp gets the original. `stolen` and `prio` are
    // Mewjector's and are ignored (MinHook sizes the steal itself). Returns 1/0.
    int (__cdecl *InstallHook)(UINT_PTR rva, int stolen, void* fn, void** tramp, int prio, const char* owner);
} MewjectorAPI;

int MJ_Require(const char* who);
int MJ_Resolve(MewjectorAPI* api);

#ifdef __cplusplus
}
#endif
