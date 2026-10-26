// pvp_glue.cpp -- the seams between mgmp and the arena (src/pvp).
//
// The arena code came from pvp/probe, a separate Mewjector mod. Here it is part of
// mgmp.dll: its hooks go through MinHook (pvp_mj_shim.h), and the two places both
// mods wanted -- the SDL swap slot and Button::update -- are mgmp's detours, which
// call into the arena (pvp_frame, pvp_button_update) instead of being hooked twice.
#include "pvp_mj_shim.h"
#include "pvp_glue.h"

#include "MinHook.h"
#include "mgmp_config.h"
#include "mgmp_lockstep.h"
#include "mgmp_log.h"
#include "mgmp_net.h"
#include "mgmp_proto.h"
#include "mgmp_session.h"

extern "C" int MJ_Require(const char*) { return 1; }

static int __cdecl shim_install(UINT_PTR rva, int, void* fn, void** tramp, int, const char* owner) {
    void* addr = (unsigned char*)GetModuleHandleW(nullptr) + rva;
    MH_STATUS s = MH_CreateHook(addr, fn, tramp);
    if (s != MH_OK) {
        mgmp::log_raw("  [!] pvp: MH_CreateHook failed (%d) at rva %08X (%s)", (int)s, (unsigned)rva, owner ? owner : "?");
        return 0;
    }
    s = MH_EnableHook(addr);
    if (s != MH_OK) {
        mgmp::log_raw("  [!] pvp: MH_EnableHook failed (%d) at rva %08X", (int)s, (unsigned)rva);
        return 0;
    }
    return 1;
}

extern "C" int MJ_Resolve(MewjectorAPI* api) {
    api->InstallHook = &shim_install;
    return 1;
}

extern "C" int pvp_net_mode(void) {
    using namespace mgmp;
    if (!config().net_pvp || !net_active() || !session_ready()) return 0;
    return net_role() == NetRole::Host ? 1 : net_role() == NetRole::Client ? 2 : 0;
}

extern "C" int pvp_net_send(uint8_t sub, const void* data, uint32_t size) {
    mgmp::PvpMsg m;
    m.sub = sub;
    m.size = size;
    m.data = (uint8_t*)data;
    return mgmp::net_send_pvp(m) ? 1 : 0;
}

extern "C" void pvp_lockstep_enter(uint64_t battle_id) { mgmp::lockstep_enter_battle(battle_id); }
extern "C" void pvp_lockstep_split(int (*owner)(const void*)) { mgmp::lockstep_set_split_owner(owner); }
extern "C" int  pvp_lockstep_halted(void) { return mgmp::lockstep_halted() ? 1 : 0; }
