// Opt-in, separately compiled sync stress harness. Never registers a cat in a save.
#include "pvp_synctest.h"
#include "pvp_clone.h"
#include "mgmp_ability.h"
#include "mgmp_log.h"
#include "mgmp_mem.h"
#include "pvp_glue.h"
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#ifdef MGMP_SYNCTEST
namespace {
struct Class { std::string name, attack; };
std::vector<Class> classes;
std::vector<std::string> actives, passives;
std::string directory, replay;
FILE* report = nullptr;
uint64_t root_seed, rng, fight_seed;
unsigned fight;
bool enabled;
int trigger_depth, choice_depth;
unsigned spawn_serial;
void trace_rng(const char* event, const char* name, void* object) {
    if (!enabled) return;
    auto tls = (unsigned char**)__readgsqword(0x58);
    uint64_t state[4] = {};
    if (tls && tls[0]) memcpy(state, tls[0] + 0x178, sizeof state);
    fprintf(report, "TRACE turn=%u event=%s name=%s depth=%d choice=%d rng=%016llx:%016llx:%016llx:%016llx object=%p\n",
            mgmp::log_turn(), event, name, trigger_depth, choice_depth,
            state[0], state[1], state[2], state[3], object);
    fflush(report);
}
uint64_t next() {
    uint64_t z = (rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
std::string choose(const std::vector<std::string>& pool) { return pool[next() % pool.size()]; }
// CatData has MSVC's 32-byte std::string. Use the game's heap on both sides.
void assign(unsigned char* dest, const std::string& value) {
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    if (*(uint64_t*)(dest + 24) > 15)
        ((void(__cdecl*)(void*))(base + 0x9DB940))(*(void**)dest);
    memset(dest, 0, 32);
    *(uint64_t*)(dest + 16) = value.size();
    if (value.size() <= 15) {
        *(uint64_t*)(dest + 24) = 15;
        memcpy(dest, value.c_str(), value.size() + 1);
    } else {
        char* text = (char*)((void*(__cdecl*)(size_t))(base + 0x52A40))(value.size() + 1);
        memcpy(text, value.c_str(), value.size() + 1);
        *(void**)dest = text;
        *(uint64_t*)(dest + 24) = value.size();
    }
}
const char* targets[][5] = {
    {"Convert", "CupidsArrow", "CharmTrap", "TurnFoe", nullptr},
    {"SpawnPooterFriend", "SpawnMaggotFriend", "SpawnTomTomFriend", "SummonBrambles", nullptr},
    {"Revive", "BornAgain", "DeepDive", "GuardianAngel", nullptr},
    {"MageTeleport", "MageSwap", "Warp", "ChaosTeleport", nullptr},
    {"Counter", "ReverseDamage", "EyeForAnEye", "DivineProtection", nullptr},
    {"ChaosShot", "ChaosRampage", "PoisonLace", "FireSurge", nullptr},
};
const char* scenarios[] = {"charm", "summons", "revival", "teleport", "reactions", "random_and_elements", "mixed"};
}
#endif

extern "C" void pvp_test_init(HMODULE self) {
#ifdef MGMP_SYNCTEST
    char path[MAX_PATH]; GetModuleFileNameA(self, path, MAX_PATH);
    directory = path; directory.resize(directory.find_last_of('\\') + 1);
    std::string ini = directory + "pvp.ini";
    if (!GetPrivateProfileIntA("synctest", "Enabled", 0, ini.c_str()) ||
        !GetPrivateProfileIntA("auto", "Enabled", 0, ini.c_str())) return;
    root_seed = (unsigned)GetPrivateProfileIntA("synctest", "Seed", 20261102, ini.c_str());
    char file[MAX_PATH]; GetPrivateProfileStringA("synctest", "ReplayStart", "", file, MAX_PATH, ini.c_str());
    replay = file;
    FILE* pool = fopen((directory + "synctest_pool.txt").c_str(), "r");
    if (!pool) { mgmp::log_line("SYNCTEST", "pool missing -- stress harness OFF"); return; }
    char line[256];
    while (fgets(line, sizeof line, pool)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'C' && line[1] == '\t') {
            char* tab = strchr(line + 2, '\t');
            if (tab) { *tab = 0; classes.push_back({line + 2, tab + 1}); }
        } else if (line[0] == 'A' && line[1] == '\t') actives.emplace_back(line + 2);
        else if (line[0] == 'P' && line[1] == '\t') passives.emplace_back(line + 2);
    }
    fclose(pool);
    enabled = !classes.empty() && actives.size() >= 4 && passives.size() >= 2;
    if (enabled) report = fopen((directory + "synctest.log").c_str(), "w");
    if (!report) enabled = false;
    mgmp::log_line("SYNCTEST", "enabled=%d seed=%llu classes=%zu skills=%zu passives=%zu", enabled, root_seed, classes.size(), actives.size(), passives.size());
#else
    (void)self;
#endif
}
extern "C" int pvp_test_manual_face(void* brain) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return 0;
    static void* last_brain;
    static unsigned last_fight, last_turn;
    unsigned turn = mgmp::log_turn();
    if (brain == last_brain && fight == last_fight && turn == last_turn) return 0;
    auto ch = *(unsigned char**)((unsigned char*)brain + 0x38);
    auto obj = *(unsigned char**)(ch + 0x60);
    int xy[2]; memcpy(xy, obj + 0x48, sizeof xy);
    int facing[2]; memcpy(facing, ch + 0x388, sizeof facing);
    xy[0] -= facing[1]; xy[1] += facing[0];
    uint64_t tile; memcpy(&tile, xy, sizeof tile);
    // Exercise the real FaceTowards hook, exactly as PlayerBrain's manual
    // click does. No simulation write here: it must arrive via queued ACTION.
    ((void(__fastcall*)(void*, uint64_t, unsigned char))((uintptr_t)GetModuleHandleW(nullptr) + 0x10DEB0))(ch, tile, 0);
    last_brain = brain; last_fight = fight; last_turn = turn;
    mgmp::log_line("SYNCTEST", "manual face input before turn %u", turn);
    return 1;
#else
    (void)brain; return 0;
#endif
}

extern "C" int pvp_test_enabled(void) {
#ifdef MGMP_SYNCTEST
    return enabled;
#else
    return 0;
#endif
}
extern "C" uint64_t pvp_test_begin(void) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return 0;
    ++fight; rng = root_seed + fight * 0x9e3779b97f4a7c15ULL; fight_seed = next();
    fprintf(report, "FIGHT %u seed=%llu scenario=%s\n", fight, fight_seed, scenarios[(fight - 1) % 7]); fflush(report);
    return fight_seed;
#else
    return 0;
#endif
}
extern "C" int pvp_test_cat(uint8_t** bytes, uint32_t* size, lobby_cat* row, int slot) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return 1;
    // Slot 15 is a host-only scratch clone, outside the arena's <=8 fighters.
    unsigned char* cat = (unsigned char*)pvp_clone_from_bytes(15, *bytes, *size);
    if (!cat) return 0;
    const Class& cls = classes[(fight + slot) % classes.size()];
    std::vector<std::string> selected;
    unsigned scenario = (fight - 1) % 7;
    if (scenario < 6) {
        std::vector<std::string> eligible;
        for (int k = 0; targets[scenario][k]; ++k)
            if (std::find(actives.begin(), actives.end(), targets[scenario][k]) != actives.end()) eligible.emplace_back(targets[scenario][k]);
        if (!eligible.empty()) selected.push_back(eligible[slot % eligible.size()]);
    }
    while (selected.size() < 4) { auto name = choose(actives); if (std::find(selected.begin(), selected.end(), name) == selected.end()) selected.push_back(name); }
    assign(cat + 0xC10, cls.name);
    assign(cat + 0x7D0, cls.attack); // preserve the donor's valid movement ability
    for (int i = 0; i < 4; ++i) {
        assign(cat + 0x810 + 32 * i, selected[i]); assign(cat + 0x890 + 32 * i, "None");
        strncpy_s(row->abil[i], selected[i].c_str(), _TRUNCATE);
    }
    auto p0 = choose(passives), p1 = choose(passives); while (p1 == p0) p1 = choose(passives);
    assign(cat + 0x910, p0); assign(cat + 0x938, p1);
    *(int64_t*)(cat + 0x930) = *(int64_t*)(cat + 0x958) = 1;
    assign(cat + 0x960, "None"); assign(cat + 0x988, "None");
    *(int64_t*)(cat + 0x980) = *(int64_t*)(cat + 0x9A8) = 1;
    memset(cat + 0x70C, 0, 0x7C); // level/injury stat deltas and injury array
    for (int i = 0; i < 7; ++i) *(int*)(cat + 0x6F0 + 4 * i) = row->stats[i] = (i == 2 || i == 3) ? 14 : 8 + (int)(next() % 5);
    *(int*)(cat + 0x7A8) = 0x3fffffff; cat[0x7AC] = 0; *(int*)(cat + 0x7B0) = 0;
    *(uint32_t*)(cat + 0xC30) = row->level = 8; row->age_days = 28;
    strncpy_s(row->cls, cls.name.c_str(), _TRUNCATE);
    strncpy_s(row->passive[0], p0.c_str(), _TRUNCATE); strncpy_s(row->passive[1], p1.c_str(), _TRUNCATE);
    fprintf(report, "CAT fight=%u slot=%d class=%s stats=", fight, slot, cls.name.c_str());
    for (int v : row->stats) fprintf(report, "%d,", v);
    fprintf(report, " skills=%s,%s,%s,%s passives=%s,%s\n", selected[0].c_str(), selected[1].c_str(), selected[2].c_str(), selected[3].c_str(), p0.c_str(), p1.c_str()); fflush(report);
    uint8_t* replacement = nullptr; uint32_t length = pvp_clone_serialize(cat, &replacement);
    if (!length) return 0;
    free(*bytes); *bytes = replacement; *size = length;
    return 1;
#else
    (void)bytes; (void)size; (void)row; (void)slot; return 1;
#endif
}
extern "C" void pvp_test_capture(const void* data, uint32_t size) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return;
    char name[64]; sprintf_s(name, "synctest_%04u.start", fight);
    FILE* out = fopen((directory + name).c_str(), "wb");
    if (out) { fwrite(data, 1, size, out); fclose(out); }
#else
    (void)data; (void)size;
#endif
}
extern "C" uint32_t pvp_test_replay(uint8_t** bytes) {
    *bytes = nullptr;
#ifdef MGMP_SYNCTEST
    if (!enabled || replay.empty()) return 0;
    FILE* in = fopen(replay.c_str(), "rb"); replay.clear();
    if (!in) return 0;
    fseek(in, 0, SEEK_END); long size = ftell(in); rewind(in);
    if (size <= 0 || size > 4 * 1024 * 1024) { fclose(in); return 0; }
    auto data = (uint8_t*)malloc(size);
    if (!data || fread(data, 1, size, in) != (size_t)size) { free(data); fclose(in); return 0; }
    fclose(in); data[0] = 0; *bytes = data; return (uint32_t)size;
#else
    return 0;
#endif
}
extern "C" void pvp_test_trigger(void* ability) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return;
    char name[128];
    if (!mgmp::ability_gon_name(ability, name, sizeof name)) strcpy_s(name, "unknown");
    fprintf(report, "CAST fight=%u ability=%s\n", fight, name); fflush(report);
    int mode = 0, x = 0;
    mgmp::mem_read((unsigned char*)ability + 0x204, &mode, sizeof mode);
    mgmp::mem_read((unsigned char*)ability + 0x6C4, &x, sizeof x);
    fprintf(report, "PARAM turn=%u name=%s mode=%d X=%d\n", mgmp::log_turn(), name, mode, x);
    trace_rng("trigger_begin", name, ability);
    ++trigger_depth;
#else
    (void)ability;
#endif
}
extern "C" void pvp_test_trigger_end(void* ability) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return;
    if (trigger_depth > 0) --trigger_depth;
    char name[128];
    if (!mgmp::ability_gon_name(ability, name, sizeof name)) strcpy_s(name, "unknown");
    trace_rng("trigger_end", name, ability);
#else
    (void)ability;
#endif
}
extern "C" void pvp_test_spawn(void* character, void* type_name) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return;
    char name[128];
    if (!mgmp::mem_read_std_string(type_name, name, sizeof name)) strcpy_s(name, "unreadable");
    fprintf(report, "SPAWN serial=%u ", ++spawn_serial);
    trace_rng("character_init", name, character);
#else
    (void)character; (void)type_name;
#endif
}
extern "C" void pvp_test_choice_scope(int on) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return;
    choice_depth += on ? 1 : -1;
    if (choice_depth < 0) choice_depth = 0;
#else
    (void)on;
#endif
}
extern "C" uint64_t pvp_test_rng_hash(void) {
#ifdef MGMP_SYNCTEST
    if (!enabled) return 0;
    auto tls = (unsigned char**)__readgsqword(0x58);
    if (!tls || !tls[0]) return 0;
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 32; ++i) { hash ^= tls[0][0x178 + i]; hash *= 0x100000001b3ULL; }
    return hash;
#else
    return 0;
#endif
}
extern "C" void pvp_test_roll_x(void* ability, uintptr_t caller, uint64_t before) {
#ifdef MGMP_SYNCTEST
    if (!enabled || before == pvp_test_rng_hash()) return;
    char name[128];
    if (!mgmp::ability_gon_name(ability, name, sizeof name)) strcpy_s(name, "unknown");
    fprintf(report, "ROLL_X caller_rva=%llx before=%016llx ", (unsigned long long)(caller - (uintptr_t)GetModuleHandleW(nullptr)), before);
    trace_rng("roll_x", name, ability);
#else
    (void)ability; (void)caller; (void)before;
#endif
}
