#include "pvp_clone.h"

#include "MinHook.h"
#include "mgmp_addresses.h"
#include "mgmp_bytestream.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "mgmp_resolve.h"

#include <windows.h>
#include <intrin.h>
#include <cstdlib>
#include <cstring>

using namespace mgmp;

namespace {

// CatData::by_id's cache-miss branch, read off FUN_1400D7220.
constexpr uintptr_t kRva_GameNew  = 0xD3E0A4;   // operator new(size)
constexpr uintptr_t kRva_CatCtor  = 0x05D6B0;   // the CatData constructor it calls
constexpr size_t    kCatDataSize  = 0xC58;
constexpr uintptr_t kCat_SqlKey   = 0xC48;      // what the run list / by_id key on
constexpr uint32_t  kMaxBytes     = 64 * 1024;

typedef void  (__fastcall* fn_serialize_cat)(void* cat, void* stream, bool flag);
typedef void  (__fastcall* fn_bs_dtor)(void* stream);
typedef void* (__fastcall* fn_ofstream_ctor)(void* self);
typedef void* (__fastcall* fn_cat_by_id)(void* registry, uint64_t id);
typedef void* (__cdecl*    fn_game_new)(size_t);
typedef void* (__fastcall* fn_cat_ctor)(void* cat);

struct Clone { void* cat; long long key; };

struct State {
    uintptr_t        base = 0;
    fn_serialize_cat serialize = nullptr;
    fn_bs_dtor       bs_dtor = nullptr;
    fn_ofstream_ctor of_ctor = nullptr;
    fn_cat_by_id     orig_by_id = nullptr;
    Clone            clones[PVP_CLONE_MAX] = {};
    bool             live = false;
} g;

bool safe_ofstream_ctor(void* p) { __try { g.of_ctor(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool safe_bs_dtor(void* p)       { __try { g.bs_dtor(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
bool safe_serialize(void* cat, void* bs) {
    __try { g.serialize(cat, bs, true); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool bs_construct(uint8_t* bs) {
    memset(bs, 0, kByteStreamSize);
    if (!safe_ofstream_ctor(bs + kBS_Ofstream)) return false;
    *(uint32_t*)(bs + kBS_SwapElem) = 8;
    return true;
}

// The simulation stream, TLS block 0 + 0x178 (32 bytes): the constructor may draw.
unsigned char* sim_stream() {
    unsigned char** tls = (unsigned char**)__readgsqword(0x58);
    return tls && tls[0] ? tls[0] + 0x178 : nullptr;
}

void* __fastcall hook_by_id(void* registry, uint64_t id) {
    if ((long long)id >= PVP_CLONE_KEY_BASE && (long long)id < PVP_CLONE_KEY_BASE + PVP_CLONE_MAX) {
        Clone& c = g.clones[(long long)id - PVP_CLONE_KEY_BASE];
        if (c.cat && c.key == (long long)id) return c.cat;
    }
    return g.orig_by_id(registry, id);
}

void* make_cat() {
    unsigned char saved[32];
    unsigned char* rng = sim_stream();
    if (rng) memcpy(saved, rng, 32);
    void* cat = nullptr;
    __try {
        cat = ((fn_game_new)(g.base + kRva_GameNew))(kCatDataSize);
        if (cat) {
            memset(cat, 0, kCatDataSize);
            ((fn_cat_ctor)(g.base + kRva_CatCtor))(cat);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cat = nullptr;
    }
    if (rng) memcpy(rng, saved, 32);
    return cat;
}

}  // namespace

extern "C" int pvp_clone_init(void) {
    g.base = (uintptr_t)GetModuleHandleW(nullptr);
    g.serialize = (fn_serialize_cat)addr_of_call(C_SerializeCatData);
    g.bs_dtor   = (fn_bs_dtor)addr_of_call(C_ByteStreamDtor);
    g.of_ctor   = (fn_ofstream_ctor)addr_of_call(C_OfstreamCtor);
    void* by_id = (void*)addr_of_call(C_CatDataById);
    if (!g.serialize || !g.bs_dtor || !g.of_ctor || !by_id) {
        log_raw("  [!] pvp clones: a call target did not resolve -- clones OFF");
        return 0;
    }
    if (MH_CreateHook(by_id, (void*)&hook_by_id, (void**)&g.orig_by_id) != MH_OK || MH_EnableHook(by_id) != MH_OK) {
        log_raw("  [!] pvp clones: could not hook CatData::by_id -- clones OFF");
        return 0;
    }
    g.live = true;
    log_raw("  pvp clones: CatData::by_id hooked at %p", by_id);
    return 1;
}

extern "C" uint32_t pvp_clone_serialize(void* cat, uint8_t** out) {
    *out = nullptr;
    if (!g.live || !cat) return 0;
    uint8_t bs[kByteStreamSize];
    if (!bs_construct(bs)) return 0;
    *(uint32_t*)(bs + kBS_Mode) = 1;   // write
    uint32_t n = 0;
    if (safe_serialize(cat, bs)) {
        const void* buf = *(const void**)(bs + kBS_WriteBuf);
        uint32_t len = *(uint32_t*)(bs + kBS_WriteLen);
        if (buf && len && len <= kMaxBytes) {
            uint8_t* copy = (uint8_t*)malloc(len);
            if (copy && mem_read(buf, copy, len)) { *out = copy; n = len; }
            else free(copy);
        }
    }
    safe_bs_dtor(bs);
    return n;
}

extern "C" void* pvp_clone_from_bytes(int slot, const uint8_t* data, uint32_t size) {
    if (!g.live || slot < 0 || slot >= PVP_CLONE_MAX || !data || !size) return nullptr;
    Clone& c = g.clones[slot];
    if (!c.cat) c.cat = make_cat();   // kept for reuse: a rematch re-reads into it
    if (!c.cat) { log_line("PVP", "!! clone %d: could not construct a CatData", slot); return nullptr; }
    uint8_t bs[kByteStreamSize];
    if (!bs_construct(bs)) return nullptr;
    *(uint32_t*)(bs + kBS_Mode) = 0;                  // read
    *(const void**)(bs + kBS_ReadBuf) = data;         // borrowed: the game must not free it
    *(uint8_t*)(bs + kBS_ReadOwns) = 0;
    *(uint32_t*)(bs + kBS_ReadLen) = size;
    *(uint32_t*)(bs + kBS_ReadPos) = 0;
    bool ok = safe_serialize(c.cat, bs);
    safe_bs_dtor(bs);
    if (!ok) { log_line("PVP", "!! clone %d: SerializeCatData faulted", slot); return nullptr; }
    c.key = PVP_CLONE_KEY_BASE + slot;
    *(long long*)((unsigned char*)c.cat + kCat_SqlKey) = c.key;
    return c.cat;
}

extern "C" long long pvp_clone_key(int slot) { return PVP_CLONE_KEY_BASE + slot; }

extern "C" void* pvp_clone_by_key(long long key) {
    if (key < PVP_CLONE_KEY_BASE || key >= PVP_CLONE_KEY_BASE + PVP_CLONE_MAX) return nullptr;
    Clone& c = g.clones[key - PVP_CLONE_KEY_BASE];
    return c.key == key ? c.cat : nullptr;
}

extern "C" void pvp_clone_clear(void) {
    for (auto& c : g.clones) c.key = 0;
}
