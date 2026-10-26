// pvp_probe -- live tests for a Mewgenics PvP mode.
//
// v1 finding: flipping the faction alone is not enough. The battle-start unit
// setup FUN_1401057D0 (virtual, no direct callers) replaces a PlayerBrain with
// `ai_if_spawned_as_enemy.brain` or GenericBrain whenever
//   FUN_14011A380(ch, 1) -- "is the base faction (+0x350) a player faction",
//                           a lookup in the 14-byte table at 0x1413C455E --
// is false and `ai.always_allow_player_control` is not set.
//
// v2 (confirmed live): post-hook the Character init (FUN_1400F70F0): for the
// 2nd, 4th, ... cat with a PlayerBrain, set the base faction (+0x350) to 2
// (enemies) and remember it. Hook the battle-start setup: for a remembered cat,
// show faction 1 (allies) for the duration of the call so the brain survives,
// then put enemies back into +0x350, +0x354 and +0x358 (the setup derives the
// last two from +0x350).
//
// v3: F9 in the house starts a fight on a cats-only arena level, through the
// function the house-boss debug buttons use (see start_arena).
//
// Loaded by Mewjector from <game>\mods. Pinned to one build: refuses to hook if
// the entry bytes do not match.

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <intrin.h>
#include <stdlib.h>
#include "pvp_mj_shim.h"
#include "pvp_glue.h"
#include "pvp_clone.h"

#include "lobby.h"
// Online (see the end of the file). Declared up here for the hotseat code that
// hands over to it.
static uint64_t fnv64(const void* p, uint32_t n, uint64_t h);
static void auto_tick(void);
static void auto_init(HMODULE self);
static void auto_button(unsigned char* btn);
static void auto_hooks(void);
static void intro_hooks(HMODULE self);
static void face_hooks(void);
static void roll_x_hooks(void);
static int  net_open_lobby(const lobby_cat* cats, int n, const lobby_settings* s);
static int  net_lobby_tick(void);
static int  net_route_action(long a);
static int  net_end_choice(int rematch);
static unsigned int rnd_fight(unsigned int n);
static char          g_level_fixed[16];     // "pvp/g.lvl" while an online START fixed the level
static unsigned char g_online_seed[32];     // the START's sim-stream seed
static int           g_online_seed_set;
static int           g_arena_online;        // the current arena came from an online START
#include "gamedata.h"
#include "lobby.h"

// The game's operator new (used wherever the game will free what we hand it).
#define RVA_GAME_NEW    0x52A40
typedef void* (__fastcall *fn_game_new)(size_t);

#define RVA_CHARACTER_INIT  0xF70F0
#define RVA_BATTLE_SETUP    0x1057D0
#define OFF_BRAIN           0x68
#define OFF_CATDATA         0x88
#define OFF_CHAR_FACING        0x388
#define OFF_FACTION_BASE    0x350
#define OFF_FACTION_354     0x354
#define OFF_FACTION_358     0x358
#define FACTION_ALLIES      1
#define FACTION_ENEMIES     2
#define FACTION_SOLITARY    3
#define FACTION_THIRD_PARTY 6

static const unsigned char kInitEntry[15] = {
    0x48,0x8B,0xC4, 0x44,0x88,0x48,0x20, 0x44,0x88,0x40,0x18, 0x48,0x89,0x50,0x10 };
static const unsigned char kSetupEntry[15] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x48,0x08, 0x55, 0x53, 0x56, 0x57, 0x41,0x54, 0x41,0x55 };

typedef void (__fastcall *fn_char_init)(void*, void*, char, char);
typedef void (__fastcall *fn_battle_setup)(void*);
static fn_char_init    g_orig_init;
static fn_battle_setup g_orig_setup;
static MewjectorAPI mj;
static FILE* g_log;
static LONG g_human_cats;
static int  g_flip_every = 2;   // 0 = log only
static int  g_freeze_ms = 700;  // banner freeze delay, see end_choice_tick

// Characters this probe flipped. Pointers are stable within one battle; a stale
// entry can only match a later Character allocated at the same address, which
// the init hook clears before it can be misread.
#define MAX_FLIPPED 64
static void* g_flipped[MAX_FLIPPED];
static CRITICAL_SECTION g_lock;

static void plog(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void flipped_set(void* ch, int on) {
    EnterCriticalSection(&g_lock);
    int free_slot = -1;
    for (int i = 0; i < MAX_FLIPPED; i++) {
        if (g_flipped[i] == ch) { if (!on) g_flipped[i] = NULL; LeaveCriticalSection(&g_lock); return; }
        if (!g_flipped[i] && free_slot < 0) free_slot = i;
    }
    if (on && free_slot >= 0) g_flipped[free_slot] = ch;
    LeaveCriticalSection(&g_lock);
}

static int flipped_has(void* ch) {
    int hit = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_FLIPPED; i++) if (g_flipped[i] == ch) { hit = 1; break; }
    LeaveCriticalSection(&g_lock);
    return hit;
}

// MSVC x64 RTTI: vtable[-1] -> CompleteObjectLocator; +0x0C type descriptor RVA,
// +0x14 the locator's own RVA (gives the image base). Name at TypeDescriptor+0x10.
static const char* rtti_name(void* obj) {
    __try {
        if (!obj) return "(null)";
        unsigned char* vt = *(unsigned char**)obj;
        unsigned char* col = *(unsigned char**)(vt - 8);
        if (*(int*)col != 1) return "?";
        unsigned char* base = col - *(int*)(col + 0x14);
        return (const char*)(base + *(int*)(col + 0x0C) + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "?";
    }
}

#define F(c, off) (*(int*)((unsigned char*)(c) + (off)))

// Arena state, shared with the referee below. Written on the game thread only
// (init hook, swap detour).
#define MAX_ARENA_CATS 16
static volatile LONG g_arena_active;
static void* g_arena_cats[MAX_ARENA_CATS];
static int   g_arena_count;

// v8: the two teams, as cat sql_keys (CatData+0xC48), chosen from two house rooms
// when F9 is pressed. Team 2 is the side that gets flipped to enemies.
#define OFF_CAT_SQL_KEY 0xC48
#define MAX_TEAM 8
static long long g_team_keys[2][MAX_TEAM];
static int       g_team_size[2];

static int team_of_cat(void* catdata) {
    long long key = *(long long*)((unsigned char*)catdata + OFF_CAT_SQL_KEY);
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < g_team_size[t]; i++)
            if (g_team_keys[t][i] == key) return t + 1;
    return 0;
}

static int g_in_level_init;
// v19.1: this fight's birds, to cap how many are alive (owner, run 43). A Character
// allocated at a freed bird's address is taken off the list by the init hook first.
#define MAX_BIRDS 32
static void* g_birds[MAX_BIRDS];
static int g_round;
static void* g_upkept[128];   // characters RoundUpkeep reached this round (online)
static int   g_upkept_n;
static unsigned long long g_last_upkeep_ms;
static volatile LONG g_wave_pending;
// Run 40: the level's monsters are created after Level::init returns, still inside
// the start call (FUN_140319170), so the window is that whole call.
static int g_spawning;
static int g_mob_count, g_mob_mode;
static int g_mobs_moved;
// v17: the monsters of this Level::init, for the Madness status afterwards.
#define MAX_MOBS 32
static void* g_mobs[MAX_MOBS];
static int   g_mob_n;

static void __fastcall hook_char_init(void* ch, void* gon, char a, char b) {
    flipped_set(ch, 0);
    for (int i = 0; i < MAX_BIRDS; i++) if (g_birds[i] == ch) g_birds[i] = NULL;
    g_orig_init(ch, gon, a, b);
    __try {
        unsigned char* c = (unsigned char*)ch;
        void* cat = *(void**)(c + OFF_CATDATA);
        const char* cls = rtti_name(*(void**)(c + OFF_BRAIN));
        int f350 = F(c, OFF_FACTION_BASE), f354 = F(c, OFF_FACTION_354);
        const char* act = "";
        int human_cat = strcmp(cls, ".?AVPlayerBrain@glaiel@@") == 0 && cat;
        if (human_cat && g_arena_active && g_arena_count < MAX_ARENA_CATS)
            g_arena_cats[g_arena_count++] = ch;
        // Only inside an arena, and only the second room's cats. Outside the arena
        // nothing is flipped, so a normal adventure is never touched.
        int team = (human_cat && g_arena_active) ? team_of_cat(cat) : 0;
        if (team == 2) {
            F(c, OFF_FACTION_BASE) = FACTION_ENEMIES;
            flipped_set(ch, 1);
            act = "  -> team 2, FLIPPED to enemies";
        } else if (team == 1) {
            act = "  -> team 1";
        } else if (!human_cat && g_arena_active && f350 == FACTION_THIRD_PARTY && !cat) {
            for (int i = 0; i < MAX_BIRDS; i++) if (!g_birds[i]) { g_birds[i] = ch; break; }
            act = "  -> bird";
        } else if (!human_cat && g_arena_active && g_spawning && g_mob_count > 0 &&
                   f350 == FACTION_ENEMIES) {
            F(c, OFF_FACTION_BASE) = g_mob_mode ? FACTION_SOLITARY : FACTION_THIRD_PARTY;
            g_mobs_moved++;
            if (g_mob_n < MAX_MOBS) g_mobs[g_mob_n++] = ch;
            act = g_mob_mode ? "  -> monster, solitary" : "  -> monster, third party";
        }
        plog("init  char=%p brain=%s cat=%p key=%lld f350=%d f354=%d%s", ch, cls, cat,
             cat ? *(long long*)((unsigned char*)cat + OFF_CAT_SQL_KEY) : -1LL, f350, f354, act);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("init  char=%p: fault while inspecting", ch);
    }
}

static void __fastcall hook_battle_setup(void* ch) {
    if (!flipped_has(ch)) { g_orig_setup(ch); return; }

    unsigned char* c = (unsigned char*)ch;
    plog("setup char=%p before: brain=%s f=%d/%d/%d", ch, rtti_name(*(void**)(c + OFF_BRAIN)),
         F(c, OFF_FACTION_BASE), F(c, OFF_FACTION_354), F(c, OFF_FACTION_358));
    F(c, OFF_FACTION_BASE) = FACTION_ALLIES;
    g_orig_setup(ch);
    int s354 = F(c, OFF_FACTION_354), s358 = F(c, OFF_FACTION_358);
    F(c, OFF_FACTION_BASE) = FACTION_ENEMIES;
    if (s354 == FACTION_ALLIES) F(c, OFF_FACTION_354) = FACTION_ENEMIES;
    if (s358 == FACTION_ALLIES) F(c, OFF_FACTION_358) = FACTION_ENEMIES;
    plog("setup char=%p after:  brain=%s f=%d/%d/%d (setup left 354=%d 358=%d)", ch,
         rtti_name(*(void**)(c + OFF_BRAIN)),
         F(c, OFF_FACTION_BASE), F(c, OFF_FACTION_354), F(c, OFF_FACTION_358), s354, s358);
}

// mgmp build: settings live in pvp.ini beside mgmp.dll.
static void pvp_ini_path(HMODULE self, char* out) {
    GetModuleFileNameA(self, out, MAX_PATH);
    char* slash = strrchr(out, '\\');
    if (slash) strcpy(slash + 1, "pvp.ini"); else strcpy(out, "pvp.ini");
}

static void read_config(HMODULE self) {
    char path[MAX_PATH];
    pvp_ini_path(self, path);
    g_flip_every = GetPrivateProfileIntA("probe", "FlipEvery", 2, path);
    g_freeze_ms = GetPrivateProfileIntA("probe", "FreezeMs", 700, path);
}

static void read_team_config(HMODULE self);

static int hook(UINT_PTR rva, const unsigned char* expect, int len, void* fn, void** orig, const char* what) {
    unsigned char* entry = (unsigned char*)GetModuleHandleA(NULL) + rva;
    if (memcmp(entry, expect, len) != 0) {
        plog("%s: entry bytes at RVA 0x%X do not match -- wrong game build, not hooking", what, (unsigned)rva);
        return 0;
    }
    if (!mj.InstallHook(rva, len, fn, orig, 50, "pvp_probe")) { plog("%s: InstallHook failed", what); return 0; }
    plog("hooked %s @ RVA 0x%X", what, (unsigned)rva);
    return 1;
}

// ---------------------------------------------------------------------------
// v3: start an arena fight from the house, on F9.
//
// The house-boss debug buttons built in HousePlaceholder::init (FUN_140314B80)
// are lambdas that call
//     FUN_140319170(HousePlaceholder*, std::string level, std::string boss_name)
// with level "house_bosses/<boss>.lvl" (resolved under levels/). We make the same
// call with our own cats-only level, which the game finds through -modpaths.
//
// Both strings are MSVC std::string passed BY VALUE: the caller builds the
// object, the callee destroys it. Keeping both under 16 chars keeps them in the
// small-string buffer, so the game's destructor has no heap block to free and
// never touches our allocator.

#define RVA_HOUSE_INIT        0x314B80
#define RVA_START_HOUSE_FIGHT 0x319170
#define RVA_MEWDIRECTOR_PTR   0x13DAC30   // MewDirector** (mgmp kRva_MewDirectorPtr)
#define RVA_SDL_SWAP_SLOT     0x12E7650   // SDL_GL_SwapWindow dynapi slot (mgmp)
#define OFF_DIR_DIRECTOR      40          // MewDirector+40 = scene Director*
#define OFF_SCENE_NAME        1208        // Scene+1208 std::string
#define OFF_SCENE_DESTROYING  1243        // u8

static const unsigned char kHouseInitEntry[14] = {
    0x48,0x89,0x5C,0x24,0x08, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56 };
static const char kArenaLevel[] = "pvp/a.lvl";
static const char kArenaName[]  = "pvp";

typedef struct { union { char buf[16]; char* ptr; } u; unsigned long long size, cap; } msvc_string;
typedef void (__fastcall *fn_house_init)(void*);
typedef void (__fastcall *fn_start_house_fight)(void*, msvc_string*, msvc_string*);
typedef int  (__cdecl    *fn_sdl_swap)(void*);
static fn_house_init        g_orig_house_init;
static fn_start_house_fight g_start_house_fight;
static fn_sdl_swap volatile g_swap_prev;
static int                  g_f9_down;

static void make_sso(msvc_string* s, const char* text) {
    size_t n = strlen(text);
    memset(s, 0, sizeof *s);
    memcpy(s->u.buf, text, n);   // n < 16 for both constants above
    s->size = n;
    s->cap = 15;
}

static int read_std_string(const unsigned char* p, char* out, size_t outsz) {
    unsigned long long size = *(unsigned long long*)(p + 0x10), cap = *(unsigned long long*)(p + 0x18);
    if (size >= outsz || cap < size) return 0;
    const char* src = cap > 15 ? *(const char**)p : (const char*)p;
    memcpy(out, src, size);
    out[size] = 0;
    return 1;
}

// A live scene by name, or NULL. Pure guarded reads of the scene list
// (MewDirector+40 = Director, Scene+1208 name, Scene+1243 being-destroyed flag).
static unsigned char* find_scene(const char* want) {
    __try {
        unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
        unsigned char* dir = *(unsigned char**)(base + RVA_MEWDIRECTOR_PTR);
        if (!dir) return NULL;
        unsigned char* director = *(unsigned char**)(dir + OFF_DIR_DIRECTOR);
        if (!director) return NULL;
        unsigned char** it = *(unsigned char***)director;
        unsigned char** end = *(unsigned char***)(director + 8);
        for (; it && it < end; ++it) {
            char name[64];
            if (*it && read_std_string(*it + OFF_SCENE_NAME, name, sizeof name) &&
                strcmp(name, want) == 0 && !(*it)[OFF_SCENE_DESTROYING])
                return *it;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return NULL;
}

static unsigned char* find_house_scene(void) { return find_scene("House"); }

static int game_has_focus(void) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// FUN_140319170 reads only four things from its first argument (decompiled):
//   +0x18  the owning entity, used for global singleton lookups
//          (FUN_1400496C0 type 0x6EC, FUN_140086E70 type 0x50D)
//   +0x20  the caller's Scene; it sets Scene+0x4DB (= +1243, "being destroyed")
//   +0x28  the scene Director, to find the "Battle" scene
//   +0x5C  debug_difficultymod (HousePlaceholder default 0)
// HousePlaceholder::init never runs in the real house (run 3), so build that
// shape: entity and director from MewDirector -- whose own ContinueAdventure
// makes the same lookups through them -- and the live House scene as the scene
// to tear down, as ContinueAdventure tears it down for a house boss.
static unsigned char g_fake_component[0x100];

// MewDirector fields FUN_140319170 overwrites (house-battle mode). Saved before
// the arena starts and put back when it ends, so nothing of the arena outlives it.
// +0x780 is the flag that makes Level::update clear victory/defeat every frame
// when no house-boss controller (component 0x4BF / 0x4C0) is in the scene.
typedef struct {
    unsigned char f780, f78c, f70c;
    int f784, f6dc;
    int p410, p428, p440;   // *(MewDirector+0x5A8) + ...
    int valid;
} director_snapshot;
static director_snapshot g_dir_snap;

static void snapshot_director(unsigned char* md) {
    unsigned char* p = *(unsigned char**)(md + 0x5A8);
    g_dir_snap.f780 = md[0x780]; g_dir_snap.f78c = md[0x78C]; g_dir_snap.f70c = md[0x70C];
    g_dir_snap.f784 = *(int*)(md + 0x784); g_dir_snap.f6dc = *(int*)(md + 0x6DC);
    g_dir_snap.p410 = *(int*)(p + 0x410); g_dir_snap.p428 = *(int*)(p + 0x428); g_dir_snap.p440 = *(int*)(p + 0x440);
    g_dir_snap.valid = 1;
}

static void restore_director(unsigned char* md) {
    if (!g_dir_snap.valid) return;
    unsigned char* p = *(unsigned char**)(md + 0x5A8);
    md[0x780] = g_dir_snap.f780; md[0x78C] = g_dir_snap.f78c; md[0x70C] = g_dir_snap.f70c;
    *(int*)(md + 0x784) = g_dir_snap.f784; *(int*)(md + 0x6DC) = g_dir_snap.f6dc;
    *(int*)(p + 0x410) = g_dir_snap.p410; *(int*)(p + 0x428) = g_dir_snap.p428; *(int*)(p + 0x440) = g_dir_snap.p440;
    g_dir_snap.valid = 0;
}

// Starts an arena fight that tears down `closing` (the House for a first fight,
// the finished Battle for a rematch). The director snapshot is taken only on the
// way in from the house, so a rematch still restores the pre-arena values.
// ---------------------------------------------------------------------------
// v14: cover. Before every fight the arena level is written anew with obstacles
// added as spawn records -- the inanimate objects of data/spawns.gon categories
// 106/107 -- placed point-symmetric through the map centre, on floor or grass
// tiles, away from the cat spawns and their neighbours. The loader reads loose
// files from -modpaths first (NOTES: loose-file override), so the result goes to
// <modpath>/levels/pvp/g.lvl next to a.lvl.
//
// .lvl: u32 version, w, h, 1, spawn_count, 0, 0; u32 w, h; length-prefixed
// "spawns.gon", "tiles.gon"; w*h u16 tile ids; 8 bytes; spawn_count records
// {u16 x, u16 y, u32 spawn id}; 4 zero bytes.

static unsigned int rnd(unsigned int n);
static int  g_cover_count;            // obstacles in all, even
static int  g_cover_kind;             // 0 mixed, 1 breakable, 2 solid
// v15: ground loot -- the game's own pickups (spawns.gon category 105), placed the
// same mirrored way. Owner, run 34: coins, catnip (mana) and food; scrap (armour)
// as an extra.
static int  g_loot_count;             // pickups in all
static int  g_loot_mask = 7;          // bit 0 coins, 1 catnip, 2 food, 3 scrap
static const unsigned int kLootIds[4] = { 5002, 5061, 5011, 5060 };   // Coin, RandomCatnipPickup, RandomFoodPickup, RandomArmorPickup
static char g_modpath[MAX_PATH];
// v16: neutral / hostile monsters. Spawned as level records in mirrored pairs, then
// moved off the enemies faction in the Character init hook (only while Level::init
// runs, so whatever a team-2 cat summons later stays on its side). Faction tables
// (FUN_140024750, emulated in Ghidra): third_party (6) is hostile to allies (1) and
// enemies (2) and not to itself; solitary_enemies (3) is hostile to 1, 2 and itself.
static int  g_mob_count;              // monsters in all, even
static int  g_mob_kind;               // 0 weak, 1 medium, 2 mixed
static int  g_mob_mode;               // 0 against everyone (third_party), 1 every one for itself (solitary)
// Owner, run 39: monsters still went for team 1 more than team 2 (the hostility checks
// FUN_140119E20 / FUN_14011A090 read the full matrices, so the bias is somewhere in
// the AI). The owner's idea: give them Madness, which makes a unit attack anyone.
static int  g_mob_mad = 1;
// v18: birds that give items. PvP copies of the bonus birds from the data overrides
// written by pvp/tools/make_pvp_data.py (faction third_party, no kill counter):
// spawn ids 9101 / 9102 / 9103 = small / medium / large. Placed one by one, away
// from the spawns.
static int  g_bird_count;             // 0..6
static int  g_bird_mask = 7;          // bit 0 small, 1 medium, 2 large (owner, run 43: checkboxes)
// Status factory FUN_140483120(std::string name /*by value*/, Character*, int stacks,
// void* source, ptr, ptr, char, char) -- the same call FUN_1405E7AD0 makes after its
// listeners; "Madness" is one of its names. Data uses `Madness 999` for "permanent".
#define RVA_ADD_STATUS 0x483120
static const unsigned int kMobWeak[]   = { 11, 12, 13, 14, 15, 20, 62 };      // Rat, Fly, Worm, MadLad, Gasser, Leaper, Bat
// Security Bot (103) left out (owner, run 38): its "ally hurt -> go for the attacker"
// AI counts team 2's cats as its allies -- probably a player / not-player test.
static const unsigned int kMobMedium[] = { 71, 72, 74, 97, 78, 107 };    // Tatters, Spookie, Reaper, Cultist, Butt Zombie, Bramble Baby

static const unsigned int kCoverBreakable[] = { 5000, 5008, 5031, 5009 };   // Rock, Small Rock, Boulder, Junk
static const unsigned int kCoverSolid[]     = { 5016, 5017, 5022 };         // Rubble, Bricks, Gravel (statics)

static void find_modpath(void) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 0; i + 1 < argc; i++)
        if (!_wcsicmp(argv[i], L"-modpaths")) {
            WideCharToMultiByte(CP_ACP, 0, argv[i + 1], -1, g_modpath, MAX_PATH, NULL, NULL);
            break;
        }
    LocalFree(argv);
    plog("cover: modpath '%s'", g_modpath);
}

static unsigned int cover_pick(void) {
    int nb = sizeof kCoverBreakable / sizeof kCoverBreakable[0], ns = sizeof kCoverSolid / sizeof kCoverSolid[0];
    if (g_cover_kind == 1) return kCoverBreakable[rnd(nb)];
    if (g_cover_kind == 2) return kCoverSolid[rnd(ns)];
    unsigned int k = rnd(nb + ns);
    return k < (unsigned)nb ? kCoverBreakable[k] : kCoverSolid[k - nb];
}

static unsigned int mob_pick(void) {
    int nw = sizeof kMobWeak / sizeof kMobWeak[0], nm = sizeof kMobMedium / sizeof kMobMedium[0];
    if (g_mob_kind == 0) return kMobWeak[rnd(nw)];
    if (g_mob_kind == 1) return kMobMedium[rnd(nm)];
    unsigned int k = rnd(nw + nm);
    return k < (unsigned)nw ? kMobWeak[k] : kMobMedium[k - nw];
}

static int bird_kind_pick(void) {   // 0 small, 1 medium, 2 large, among the enabled ones
    int k[3], n = 0;
    for (int i = 0; i < 3; i++) if (g_bird_mask & (1 << i)) k[n++] = i;
    return n ? k[rnd(n)] : 0;
}

static unsigned int rnd_fight(unsigned int n);
static int bird_kind_pick_fight(void) {   // online: from the fight's seeded generator
    int k[3], n = 0;
    for (int i = 0; i < 3; i++) if (g_bird_mask & (1 << i)) k[n++] = i;
    return n ? k[rnd_fight(n)] : 0;
}

static unsigned int bird_pick(void) {
    return 9101 + bird_kind_pick();
}

static unsigned int loot_pick(void) {
    unsigned int ids[4]; int n = 0;
    for (int k = 0; k < 4; k++) if (g_loot_mask & (1 << k)) ids[n++] = kLootIds[k];
    return n ? ids[rnd(n)] : kLootIds[0];
}

// Returns the level path for this fight: "pvp/g.lvl" with cover / loot, else the plain arena.
static const char* arena_level_for_fight(void) {
    if (g_level_fixed[0]) return g_level_fixed;   // online: the host's level, already written
    static unsigned char buf[65536], out[65536 + 8 * 128];
    if ((g_cover_count <= 0 && g_mob_count <= 0 && g_bird_count <= 0 && (g_loot_count <= 0 || !g_loot_mask)) ||
        !g_modpath[0]) return kArenaLevel;
    char src[MAX_PATH], dst[MAX_PATH];
    _snprintf(src, MAX_PATH, "%s\\levels\\pvp\\a.lvl", g_modpath);
    _snprintf(dst, MAX_PATH, "%s\\levels\\pvp\\g.lvl", g_modpath);
    FILE* f = fopen(src, "rb");
    if (!f) { plog("cover: cannot read %s", src); return kArenaLevel; }
    int len = (int)fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (len < 64) return kArenaLevel;
    int w = *(int*)(buf + 4), h = *(int*)(buf + 8), n = *(int*)(buf + 16);
    int p = 36;
    for (int k = 0; k < 2 && p + 4 <= len; k++) p += 4 + *(int*)(buf + p);
    int rec = len - 4 - 8 * n;
    if (w <= 0 || h <= 0 || w > 64 || h > 64 || p + 2 * w * h > rec || rec < 0) {
        plog("cover: unexpected level layout (w %d h %d n %d)", w, h, n);
        return kArenaLevel;
    }
    const unsigned short* grid = (const unsigned short*)(buf + p);
    unsigned char blocked[64 * 64];
    for (int i = 0; i < w * h; i++)   // floor (0), grass (2), tall grass (3) only
        blocked[i] = !(grid[i] == 0 || grid[i] == 2 || grid[i] == 3);
    for (int r = 0; r < n; r++) {     // spawns and their 8 neighbours stay free
        int sx = *(unsigned short*)(buf + rec + 8 * r), sy = *(unsigned short*)(buf + rec + 8 * r + 2);
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int x = sx + dx, y = sy + dy;
                if (x >= 0 && y >= 0 && x < w && y < h) blocked[y * w + x] = 1;
            }
    }
    // Pickups may lie next to a spawn but not on one: remember the spawn tiles alone.
    unsigned char spawn_tile[64 * 64];
    memset(spawn_tile, 0, sizeof spawn_tile);
    for (int r = 0; r < n; r++)
        spawn_tile[*(unsigned short*)(buf + rec + 8 * r + 2) * w + *(unsigned short*)(buf + rec + 8 * r)] = 1;
    int want = (g_cover_count + 1) / 2, placed = 0;
    if (want > 24) want = 24;
    int mob_want = g_mob_count / 2, mob_placed = 0;
    if (mob_want > 8) mob_want = 8;
    int mob_odd = g_mob_count & 1;   // owner, run 51: an odd monster, placed alone
    int bird_want = g_bird_count, bird_placed = 0, mob_placed_single = 0;
    if (bird_want > 8) bird_want = 8;
    int loot_want = g_loot_mask ? g_loot_count : 0, loot_placed = 0;
    if (loot_want > 32) loot_want = 32;
    memcpy(out, buf, rec + 8 * n);
    int q = rec + 8 * n;
    unsigned char used[64 * 64];      // anything placed here, cover or loot
    memset(used, 0, sizeof used);
    for (int tries = 0; placed + mob_placed + bird_placed + loot_placed < want + mob_want + bird_want + loot_want + mob_odd &&
                        tries < 2000; tries++) {
        int x = rnd(w), y = rnd(h), mx = w - 1 - x, my = h - 1 - y;
        // cover, then monsters (mirrored pairs), then birds, then loot (one by one)
        int mob = placed >= want && mob_placed < mob_want;
        int mob1 = placed >= want && !mob && mob_odd;
        int bird = placed >= want && !mob && !mob1 && bird_placed < bird_want;
        int loot = placed >= want && !mob && !mob1 && !bird;
        int single = mob1 || bird || loot;
        if (!single && x == mx && y == my) continue;
        if (used[y * w + x] || (!single && used[my * w + mx])) continue;
        if (!single && (blocked[y * w + x] || blocked[my * w + mx])) continue;
        if ((bird || mob1) && blocked[y * w + x]) continue;
        if (loot && (spawn_tile[y * w + x] ||
                     !(grid[y * w + x] == 0 || grid[y * w + x] == 2 || grid[y * w + x] == 3))) continue;
        unsigned int id = loot ? loot_pick() : bird ? bird_pick() : (mob || mob1) ? mob_pick() : cover_pick();
        if (single) {   // owner, run 35: loot anywhere, one by one, not mirrored
            *(unsigned short*)(out + q) = (unsigned short)x;
            *(unsigned short*)(out + q + 2) = (unsigned short)y;
            *(unsigned int*)(out + q + 4) = id;
            q += 8;
            used[y * w + x] = 1;
            if (mob1) { blocked[y * w + x] = 1; mob_odd = 0; mob_placed_single++; }
            else if (bird) { blocked[y * w + x] = 1; bird_placed++; } else loot_placed++;
            continue;
        }
        int pts[2][2] = { { x, y }, { mx, my } };
        for (int k = 0; k < 2; k++) {
            *(unsigned short*)(out + q) = (unsigned short)pts[k][0];
            *(unsigned short*)(out + q + 2) = (unsigned short)pts[k][1];
            *(unsigned int*)(out + q + 4) = id;
            q += 8;
            blocked[pts[k][1] * w + pts[k][0]] = 1;
            used[pts[k][1] * w + pts[k][0]] = 1;
        }
        if (mob) mob_placed++; else placed++;
    }
    memcpy(out + q, buf + len - 4, 4);
    q += 4;
    *(int*)(out + 16) = n + 2 * placed + 2 * mob_placed + mob_placed_single + bird_placed + loot_placed;
    f = fopen(dst, "wb");
    if (!f) { plog("cover: cannot write %s", dst); return kArenaLevel; }
    fwrite(out, 1, q, f);
    fclose(f);
    plog("cover: %d obstacle(s) (kind %d), %d monster(s) (kind %d mode %d), %d bird(s), %d pickup(s) (mask %d) -> %s",
         2 * placed, g_cover_kind, 2 * mob_placed, g_mob_kind, g_mob_mode, bird_placed, loot_placed, g_loot_mask, dst);
    return "pvp/g.lvl";
}

// v17: Madness on this fight's monsters (see g_mob_mad).
static void give_mobs_madness(void) {
    if (!g_arena_active || !g_mob_mad || !g_mob_n) return;
    typedef void* (__fastcall *fn_add_status)(msvc_string*, void*, int, void*, void*, void*, char, char);
    fn_add_status add = (fn_add_status)((unsigned char*)GetModuleHandleA(NULL) + RVA_ADD_STATUS);
    int ok = 0;
    for (int i = 0; i < g_mob_n; i++) {
        __try {
            msvc_string name;
            make_sso(&name, "Madness");          // the callee destroys it
            add(&name, g_mobs[i], 999, NULL, NULL, NULL, 0, 0);
            ok++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            plog("mobs: fault giving Madness to %p", g_mobs[i]);
        }
    }
    plog("mobs: Madness 999 on %d of %d monster(s)", ok, g_mob_n);
}

static void start_fight(unsigned char* closing, int from_house) {
    unsigned char* md = *(unsigned char**)((unsigned char*)GetModuleHandleA(NULL) + RVA_MEWDIRECTOR_PTR);
    if (!md) { plog("start: no MewDirector, ignored"); return; }
    memset(g_fake_component, 0, sizeof g_fake_component);
    *(void**)(g_fake_component + 0x18) = *(void**)(md + 0x18);
    *(void**)(g_fake_component + 0x20) = closing;
    *(void**)(g_fake_component + 0x28) = *(void**)(md + 0x28);
    *(int*)(g_fake_component + 0x5C) = 0;
    InterlockedExchange(&g_human_cats, 0);   // flip parity restarts for this fight
    g_arena_count = 0;
    if (from_house) snapshot_director(md);
    InterlockedExchange(&g_arena_active, 1);
    msvc_string level, name;
    const char* lvl = arena_level_for_fight();
    make_sso(&level, lvl);
    make_sso(&name, kArenaName);
    plog("start: arena '%s' closing %s scene %p (MewDirector %p)",
         lvl, from_house ? "House" : "Battle", closing, md);
    g_mob_n = 0;
    memset(g_birds, 0, sizeof g_birds);
    g_round = 0;
    g_last_upkeep_ms = 0;
    g_upkept_n = 0;
    InterlockedExchange(&g_wave_pending, 0);
    g_spawning = 1;
    g_start_house_fight(g_fake_component, &level, &name);
    g_spawning = 0;
    give_mobs_madness();
    plog("start: call returned");
}

// ---------------------------------------------------------------------------
// v8: the players' own cats, two rooms = two teams.
//
// Level::init (FUN_14035C3E0) spawns the cats in MewDirector's run-cat list
// (+0x5B8 cap u32, +0x5BC count u32, +0x5C0 int64 ids; mgmp's "+1464 run's cat
// ids"). In house-battle mode (+0x780) it first calls
// MewDirector::DebugGenAdventureCats (FUN_1403B24C0), which clears that list and
// fills it with `debug_teamsize` freshly generated cats.
// Those generated cats were what leaked into the save on exit.
//
// NOT +0x781: run 9 showed the whole spawn block of Level::init sits inside
// `if (MewDirector+0x781 == 0)`, so setting it spawned no cats at all.
// Instead, for the arena: hook DebugGenAdventureCats and skip it while our list is
// installed, and put the chosen cats' ids in the list, in a buffer from the game's
// own CRT (_realloc_base) because the game may realloc it. Everything is put back
// when the arena ends.
//
// House cats: every HouseCat component in the House scene (Scene+0x18 ->
// podvector of all components). HouseCat+0xE8 = HouseRoom* (NULL for the stray
// outside), HouseRoom+0x40 = room name; the CatParts on the same entity
// (Entity+0x20 components podvector) holds CatData* at +0x8A8. Offsets from
// mewgenics-cat-bridge for this build; components told apart by vtable.
//
// Friendly fights: before the arena each chosen cat's battle-relevant CatData
// fields are copied and written back afterwards -- stats_delta_injuries and
// injuries[16] (+0x728..+0x788) and the campaign stats head (+0x7A8 hp,
// +0x7AC dead, +0x7B0; to +0x7B8). All plain data; strings and vectors after
// them are not touched (checked by a save diff after exit instead).

#define RVA_VT_HOUSECAT   0xEFD038
#define RVA_VT_CATPARTS   0xED6A30
#define RVA_REALLOC       0xBFEF10   // _realloc_base (thunk) of the game's CRT
#define OFF_HOUSECAT_ROOM 0xE8
#define OFF_ROOM_NAME     0x40
#define OFF_CATPARTS_CAT  0x8A8
#define OFF_RUNCATS       0x5B8      // {u32 cap, u32 count, int64* ids}
#define RVA_GEN_CATS      0x3B24C0   // MewDirector::DebugGenAdventureCats(md, TeamType, int)
#define CAT_SNAP_A        0x728
#define CAT_SNAP_A_LEN    0x60
#define CAT_SNAP_B        0x7A8
#define CAT_SNAP_B_LEN    0x10
#define CAT_SNAP_STR      0x788      // last_injury_debuffed_stat, std::string
// Run 13's save diff: battle scars. One byte per hurt cat changed after exit, the
// scar_sprite_idx of one body part. BodyParts sits at CatData+0x60; after a 0x2C
// header come 14 BodyPartDescriptor (0x54 each) with scar_sprite_idx at +0xC
// (mewgenics-cat-bridge glaiel_cat.hpp). Only those 14 u32 are saved: the
// descriptors also hold undecoded bytes, so the struct is not copied wholesale.
#define CAT_SCAR_FIRST    (0x60 + 0x2C + 0xC)
#define CAT_SCAR_STRIDE   0x54
#define CAT_SCAR_COUNT    14
#define OFF_ADV_STATE     0x588      // MewDirector+0x588 -> adventure inventory
// Its serializer FUN_1402E1BF0 writes adventure_coins (+0x148; Level::init sets 20
// in house-battle mode), adventure_food (+0x14C; run 14: food picked up on the
// arena, 0 -> 2) and adventure_furniture_boxes (+0x150). Saved as one block.
#define OFF_ADV_COUNTERS  0x148
#define ADV_COUNTERS_LEN  12
// Run 14: CatData.flags (u64 at +0xBF8) got bit 18 (0x40000) on a cat knocked out
// of the fight by an effect; run-end tests exactly that bit before putting a cat
// back in the house. Saved and restored with the rest.
#define CAT_FLAGS         0xBF8
#define MAX_ROOMS         16

typedef void* (__cdecl *fn_realloc)(void*, size_t);

static char g_team_room[2][64];
static int  g_team_max = 4;

typedef struct {
    unsigned char* cat;
    unsigned char a[CAT_SNAP_A_LEN], b[CAT_SNAP_B_LEN];
    unsigned int scars[CAT_SCAR_COUNT];
    unsigned long long flags;
    unsigned char str[32];   // only when the string is in its small buffer (cap 15)
    int str_ok;
} cat_snapshot;
static unsigned char g_adv_saved[ADV_COUNTERS_LEN];
static int g_adv_valid;
static cat_snapshot g_cat_snaps[2 * MAX_TEAM];
static int          g_cat_snap_count;

static struct {
    unsigned int cap, count;
    long long* ids;
    long long* ours;
    int valid;
} g_run_snap;

typedef struct { char name[64]; unsigned char* cats[2 * MAX_TEAM]; int n; } room_cats;

static unsigned char* catdata_of_housecat(unsigned char* hc, unsigned char* base) {
    unsigned char* ent = *(unsigned char**)(hc + 0x18);
    if (!ent) return NULL;
    unsigned int n = *(unsigned int*)(ent + 0x24);
    unsigned char** comps = *(unsigned char***)(ent + 0x28);
    for (unsigned int i = 0; i < n; i++)
        if (comps[i] && *(void**)comps[i] == base + RVA_VT_CATPARTS)
            return *(unsigned char**)(comps[i] + OFF_CATPARTS_CAT);
    return NULL;
}

// Groups the house's cats by room. Returns the number of rooms found.
static int collect_rooms(unsigned char* house, room_cats* rooms) {
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    int nrooms = 0;
    __try {
        unsigned char* list = *(unsigned char**)(house + 0x18);
        unsigned int n = *(unsigned int*)(list + 4);
        unsigned char** comps = *(unsigned char***)(list + 8);
        for (unsigned int i = 0; i < n; i++) {
            unsigned char* c = comps[i];
            if (!c || *(void**)c != base + RVA_VT_HOUSECAT) continue;
            unsigned char* room = *(unsigned char**)(c + OFF_HOUSECAT_ROOM);
            unsigned char* cat = room ? catdata_of_housecat(c, base) : NULL;
            char name[64];
            if (!cat || !read_std_string(room + OFF_ROOM_NAME, name, sizeof name)) continue;
            if (cat[0x7AC]) continue;   // dead
            int r = 0;
            while (r < nrooms && strcmp(rooms[r].name, name) != 0) r++;
            if (r == nrooms) {
                if (nrooms == MAX_ROOMS) continue;
                strcpy(rooms[r].name, name); rooms[r].n = 0; nrooms++;
            }
            if (rooms[r].n < 2 * MAX_TEAM) rooms[r].cats[rooms[r].n++] = cat;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("teams: fault walking the house components");
        return 0;
    }
    return nrooms;
}

// Records a cat as team t's i-th and keeps what the arena may change about it.
static void snapshot_cat(unsigned char* cat, int t, int i) {
    g_team_keys[t][i] = *(long long*)(cat + OFF_CAT_SQL_KEY);
    cat_snapshot* s = &g_cat_snaps[g_cat_snap_count++];
    s->cat = cat;
    memcpy(s->a, cat + CAT_SNAP_A, CAT_SNAP_A_LEN);
    memcpy(s->b, cat + CAT_SNAP_B, CAT_SNAP_B_LEN);
    // Run 10's save diff: the only cat field left changed after exit was
    // last_injury_debuffed_stat ("none" -> "lck"/"spd"/"str"). Stat names fit
    // the 15-char small buffer, so the string is restored byte for byte when
    // both the saved and the current value are small; otherwise left alone.
    for (int p = 0; p < CAT_SCAR_COUNT; p++)
        s->scars[p] = *(unsigned int*)(cat + CAT_SCAR_FIRST + p * CAT_SCAR_STRIDE);
    s->flags = *(unsigned long long*)(cat + CAT_FLAGS);
    s->str_ok = *(unsigned long long*)(cat + CAT_SNAP_STR + 0x18) == 15;
    if (s->str_ok) memcpy(s->str, cat + CAT_SNAP_STR, 32);
}

static int pick_teams(unsigned char* house) {
    room_cats rooms[MAX_ROOMS];
    int nrooms = collect_rooms(house, rooms);
    for (int r = 0; r < nrooms; r++) plog("teams: room '%s' has %d living cat(s)", rooms[r].name, rooms[r].n);
    int pick[2] = { -1, -1 };
    for (int t = 0; t < 2; t++) {
        for (int r = 0; r < nrooms && g_team_room[t][0]; r++)
            if (strcmp(rooms[r].name, g_team_room[t]) == 0 && rooms[r].n > 0) pick[t] = r;
        // Not configured or empty: the first other non-empty room.
        for (int r = 0; r < nrooms && pick[t] < 0; r++)
            if (rooms[r].n > 0 && r != pick[1 - t]) pick[t] = r;
    }
    if (pick[0] < 0 || pick[1] < 0 || pick[0] == pick[1]) {
        plog("teams: need two rooms with cats, found %d -- arena not started", nrooms);
        return 0;
    }
    g_cat_snap_count = 0;
    for (int t = 0; t < 2; t++) {
        room_cats* rc = &rooms[pick[t]];
        int take = rc->n < g_team_max ? rc->n : g_team_max;
        if (take > MAX_TEAM) take = MAX_TEAM;
        g_team_size[t] = take;
        for (int i = 0; i < take; i++) {
            unsigned char* cat = rc->cats[i];
            snapshot_cat(cat, t, i);
            plog("teams: player %d <- cat key %lld from '%s'", t + 1, g_team_keys[t][i], rc->name);
        }
    }
    return 1;
}

static void restore_cats(void) {
    __try {
        for (int i = 0; i < g_cat_snap_count; i++) {
            memcpy(g_cat_snaps[i].cat + CAT_SNAP_A, g_cat_snaps[i].a, CAT_SNAP_A_LEN);
            memcpy(g_cat_snaps[i].cat + CAT_SNAP_B, g_cat_snaps[i].b, CAT_SNAP_B_LEN);
            for (int p = 0; p < CAT_SCAR_COUNT; p++)
                *(unsigned int*)(g_cat_snaps[i].cat + CAT_SCAR_FIRST + p * CAT_SCAR_STRIDE) = g_cat_snaps[i].scars[p];
            unsigned long long* fl = (unsigned long long*)(g_cat_snaps[i].cat + CAT_FLAGS);
            if (*fl != g_cat_snaps[i].flags)
                plog("teams: cat %d flags %#llx -> %#llx (restored)", i, *fl, g_cat_snaps[i].flags);
            *fl = g_cat_snaps[i].flags;
            unsigned char* str = g_cat_snaps[i].cat + CAT_SNAP_STR;
            if (g_cat_snaps[i].str_ok && *(unsigned long long*)(str + 0x18) == 15)
                memcpy(str, g_cat_snaps[i].str, 32);
            else if (g_cat_snaps[i].str_ok)
                plog("teams: cat %d's last_injury_debuffed_stat went to the heap, left as is", i);
        }
        plog("teams: restored %d cat(s)", g_cat_snap_count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("teams: fault restoring cats");
    }
}

static int install_run_list(unsigned char* md) {
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    int n = g_team_size[0] + g_team_size[1];
    long long* ids = (long long*)((fn_realloc)(base + RVA_REALLOC))(NULL, n * sizeof(long long));
    if (!ids) { plog("teams: allocation failed"); return 0; }
    int k = 0;
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < g_team_size[t]; i++) ids[k++] = g_team_keys[t][i];
    unsigned char* rl = md + OFF_RUNCATS;
    g_run_snap.cap = *(unsigned int*)rl;
    g_run_snap.count = *(unsigned int*)(rl + 4);
    g_run_snap.ids = *(long long**)(rl + 8);
    g_run_snap.ours = ids;
    g_run_snap.valid = 1;
    unsigned char* adv = *(unsigned char**)(md + OFF_ADV_STATE);
    g_adv_valid = adv != NULL;
    if (adv) memcpy(g_adv_saved, adv + OFF_ADV_COUNTERS, ADV_COUNTERS_LEN);
    *(unsigned int*)rl = n;
    *(unsigned int*)(rl + 4) = n;
    *(long long**)(rl + 8) = ids;
    plog("teams: run list set to %d cat(s) (was %u, cap %u, %p)", n, g_run_snap.count, g_run_snap.cap, g_run_snap.ids);
    return 1;
}

static void restore_run_list(unsigned char* md) {
    if (!g_run_snap.valid) return;
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    unsigned char* rl = md + OFF_RUNCATS;
    // The game may have grown the buffer meanwhile; free whatever is there now.
    void* now = *(void**)(rl + 8);
    if (now && now != g_run_snap.ids) ((fn_realloc)(base + RVA_REALLOC))(now, 0);
    *(unsigned int*)rl = g_run_snap.cap;
    *(unsigned int*)(rl + 4) = g_run_snap.count;
    *(long long**)(rl + 8) = g_run_snap.ids;
    g_run_snap.valid = 0;
    unsigned char* adv = *(unsigned char**)(md + OFF_ADV_STATE);
    if (g_adv_valid && adv) {
        int* now = (int*)(adv + OFF_ADV_COUNTERS);
        int* was = (int*)g_adv_saved;
        plog("teams: adventure coins/food/boxes %d/%d/%d -> %d/%d/%d (restored)",
             now[0], now[1], now[2], was[0], was[1], was[2]);
        memcpy(adv + OFF_ADV_COUNTERS, g_adv_saved, ADV_COUNTERS_LEN);
    }
    g_adv_valid = 0;
    plog("teams: run list restored (%u cat(s))", g_run_snap.count);
}

typedef void (__fastcall *fn_gen_cats)(void*, int, unsigned int);
static fn_gen_cats g_orig_gen_cats;
static const unsigned char kGenCatsEntry[15] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x18, 0x89,0x50,0x10, 0x55, 0x56, 0x57, 0x41,0x54 };

static void __fastcall hook_gen_cats(void* md, int team_type, unsigned int count) {
    if (g_arena_active && g_run_snap.valid) {
        plog("teams: skipped DebugGenAdventureCats(type %d, %u) -- the arena brings its own cats",
             team_type, count);
        return;
    }
    g_orig_gen_cats(md, team_type, count);
}

// The arena start (FUN_140319170) re-seeds the simulation RNG at TLS+0x178
// (xoshiro256, 32 bytes -- the save's `random_seed` property), and the control run
// showed a plain exit leaves `random_seed` alone, so after an arena the save would
// carry a different seed and future rolls (events, strays) would differ. Kept and
// put back. The executable's TLS block is index 0 (`*ThreadLocalStoragePointer`
// in every decompiled access); both calls run on the game thread (swap detour).
#define TLS_SIM_RNG 0x178
static unsigned char g_rng_saved[32];
static int g_rng_valid;

static unsigned char* sim_rng(void) {
    // gs:[0x58] = ThreadLocalStoragePointer, an array of per-module TLS blocks.
    unsigned char** blocks = (unsigned char**)__readgsqword(0x58);
    if (!blocks || !blocks[0]) return NULL;
    return blocks[0] + TLS_SIM_RNG;
}

static void save_rng(void) {
    __try {
        unsigned char* r = sim_rng();
        if (r) { memcpy(g_rng_saved, r, 32); g_rng_valid = 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_rng_valid = 0; }
    plog("rng: %s", g_rng_valid ? "saved" : "NOT saved");
}

static void restore_rng(void) {
    if (!g_rng_valid) return;
    __try {
        unsigned char* r = sim_rng();
        if (r) { memcpy(r, g_rng_saved, 32); plog("rng: restored"); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { plog("rng: fault restoring"); }
    g_rng_valid = 0;
}

// ---------------------------------------------------------------------------
// v9: each team starts on its own side.
//
// Level::init (FUN_14035C3E0) collects the level's PlayerSpawn tiles in record
// order, shuffles the run-cat list with the sim RNG (FUN_140086F00), sorts it, and
// for each cat asks TacticsGrid::closest_open_tile(spawn list, ...) for a tile and
// places the cat's TacticsObject (Character+0x60) there with
// FUN_140832160(TacticsObject*, iVec2D tile, 0, 0). So which side a cat lands on
// is random. While Level::init runs, that placement call is intercepted for arena
// cats: a cat on the other team's side is moved to a free spawn of its own side.
//
// The sides are the arena's own spawn records (make_arena.py --mirror): team 1 the
// donor level's four cat spawns, team 2 those mirrored through the map centre.
// If the game's tile coordinates turn out not to be the level's (an offset), the
// tile is not one of these and nothing is changed -- only logged.

#define RVA_LEVEL_INIT 0x35C3E0
#define RVA_PLACE_TILE 0x832160
#define OFF_CHAR_TOBJ  0x60
static const unsigned char kLevelInitEntry[15] = {
    0x48,0x89,0x4C,0x24,0x08, 0x55, 0x53, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56 };
static const unsigned char kPlaceTileEntry[15] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x10, 0x44,0x88,0x40,0x18, 0x48,0x89,0x48,0x08 };
static const int kSide[2][4][2] = {
    { {3,8}, {1,8}, {2,7}, {1,6} },   // team 1
    { {6,1}, {8,1}, {7,2}, {8,3} },   // team 2
};
typedef void (__fastcall *fn_level_init)(void*);
typedef void (__fastcall *fn_place_tile)(void*, unsigned long long, char, char);
static fn_level_init g_orig_level_init;
static fn_place_tile g_orig_place_tile;
static int g_in_level_init;
static int g_side_used[2][4];

static int side_of_tile(int x, int y, int* slot) {
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 4; i++)
            if (kSide[t][i][0] == x && kSide[t][i][1] == y) { if (slot) *slot = i; return t + 1; }
    return 0;
}

// Starting money (v12, see start_arena_with_teams); declared early for this hook.
static int g_money_on, g_money;

static void __fastcall hook_level_init(void* level) {
    memset(g_side_used, 0, sizeof g_side_used);
    if (g_arena_active && g_online_seed_set) {   // online: both machines from the same sim stream
        __try {
            unsigned char* r = sim_rng();
            if (r) { memcpy(r, g_online_seed, 32); plog("net: sim stream set from the START seed"); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_in_level_init = 1;
    g_orig_level_init(level);
    g_in_level_init = 0;

    // Owner, run 36: without "starting money" the fight starts with 0, not the 20
    // Level::init gives in house-battle mode.
    if (g_arena_active) {
        __try {
            unsigned char* md = *(unsigned char**)((unsigned char*)GetModuleHandleA(NULL) + RVA_MEWDIRECTOR_PTR);
            unsigned char* adv = md ? *(unsigned char**)(md + OFF_ADV_STATE) : NULL;
            if (adv) {
                int m = g_money_on ? g_money : 0;
                *(int*)(adv + OFF_ADV_COUNTERS) = m;
                plog("money: set to %d", m);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static void __fastcall hook_place_tile(void* tobj, unsigned long long tile, char a, char b) {
    if (g_in_level_init && g_arena_active) {
        __try {
            unsigned char* ch = NULL;
            for (int i = 0; i < g_arena_count; i++)
                if (*(void**)((unsigned char*)g_arena_cats[i] + OFF_CHAR_TOBJ) == tobj) ch = g_arena_cats[i];
            unsigned char* cat = ch ? *(unsigned char**)(ch + OFF_CATDATA) : NULL;
            int team = cat ? team_of_cat(cat) : 0;
            if (team) {
                int x = (int)(tile & 0xFFFFFFFF), y = (int)(tile >> 32), slot = -1;
                int side = side_of_tile(x, y, &slot);
                if (!side) {
                    plog("spawn: team %d cat at (%d,%d), not an arena spawn -- left alone", team, x, y);
                } else if (side == team && !g_side_used[team - 1][slot]) {
                    g_side_used[team - 1][slot] = 1;
                    plog("spawn: team %d cat at (%d,%d), already its side", team, x, y);
                } else {
                    int pick = -1;
                    for (int i = 0; i < 4 && pick < 0; i++) if (!g_side_used[team - 1][i]) pick = i;
                    if (pick >= 0) {
                        g_side_used[team - 1][pick] = 1;
                        int nx = kSide[team - 1][pick][0], ny = kSide[team - 1][pick][1];
                        plog("spawn: team %d cat moved (%d,%d) -> (%d,%d)", team, x, y, nx, ny);
                        tile = ((unsigned long long)(unsigned int)ny << 32) | (unsigned int)nx;
                    } else {
                        plog("spawn: team %d has no free spawn left, cat stays at (%d,%d)", team, x, y);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            plog("spawn: fault while placing, left alone");
        }
    }
    g_orig_place_tile(tobj, tile, a, b);
}

// ---------------------------------------------------------------------------
// v11: item presets for the arena -- none, random, house.
//
// House cats wear nothing (save: only dead / retired adventurers carry items);
// items live in the house inventory and go on at adventure start. The arena puts
// items straight into the chosen cats' CatData slots and takes them out again
// afterwards; the real inventory is never touched.
//
//   CatData slots: head +0x9B0, face +0xA10, neck +0xA70, weapon +0xAD0,
//   trinket +0xB30, one Equipment (0x60 bytes) each: int64 id (-1 = empty), std::string
//   name +0x08, std::string aux +0x28, uses +0x48 ... (FUN_1400CA5C0 tests
//   `id == -1 || name.size == 0` for "empty").
//   Item constructor FUN_1402E1320(_, Equipment* out, std::string name /*by value*/)
//   takes a fresh id from a counter and fills the rest from the item data
//   (FUN_1402E01D0). FUN_1402E0030(Equipment*) = slot index (same order).
//   Inventory: *(void**)(base + 0x13C7560); +0x78 = storage, an unordered_map<id,
//   Equipment> whose list head is at +8 (node: next +0, item +0x18), size +0x10.
//
// Equipment holds std::strings with no self-pointers, so a slot's 0x60 bytes can be
// moved out and back by memcpy (that moves ownership of any heap buffer with them).
// Strings this probe creates are freed with the game's own free (thunk 0x9DB940).

#define RVA_ITEM_CTOR     0x2E1320
#define RVA_ITEM_SLOT     0x2E0030
#define RVA_INVENTORY_PTR 0x13C7560
#define RVA_GAME_FREE     0x9DB940
#define OFF_CAT_SLOT0     0x9B0
#define EQUIP_SIZE        0x60
#define NUM_SLOTS         5
enum { PRESET_NONE, PRESET_RANDOM, PRESET_HOUSE, PRESET_BALANCED };
static int g_item_preset = PRESET_NONE, g_items_per_cat = 2, g_items_mirror = 1;
// Test aid (owner, run 27): `ForceItem = <name>` gives every arena cat that item on
// top of the preset -- e.g. UraniumRod, to see whether mutations reach the save.
static char g_force_item[48];

typedef void* (__fastcall *fn_item_ctor)(void*, void*, msvc_string*, void*);
typedef int   (__fastcall *fn_item_slot)(void*);
typedef void  (__fastcall *fn_game_free)(void*);

typedef struct { unsigned char* cat; int slot; unsigned char saved[EQUIP_SIZE]; } slot_stash;
static slot_stash g_stash[2 * MAX_TEAM * NUM_SLOTS];
static int g_stash_count;

static unsigned long long g_rng_state;
static unsigned int rnd(unsigned int n) {
    if (!g_rng_state) g_rng_state = GetTickCount64() ^ (__rdtsc() << 1) | 1;
    g_rng_state ^= g_rng_state << 13; g_rng_state ^= g_rng_state >> 7; g_rng_state ^= g_rng_state << 17;
    return n ? (unsigned int)(g_rng_state % n) : 0;
}

static void make_empty_equipment(unsigned char* e) {
    memset(e, 0, EQUIP_SIZE);
    *(long long*)e = -1;
    *(unsigned long long*)(e + 0x08 + 0x18) = 15;   // name: empty small string
    *(unsigned long long*)(e + 0x28 + 0x18) = 15;   // aux
    *(int*)(e + 0x48) = -1;
    *(int*)(e + 0x50) = 6;
    *(int*)(e + 0x54) = 9999999;
    e[0x58] = 0xFF;
}

static void free_std_string(unsigned char* str) {
    unsigned long long cap = *(unsigned long long*)(str + 0x18);
    if (cap > 15) ((fn_game_free)((unsigned char*)GetModuleHandleA(NULL) + RVA_GAME_FREE))(*(void**)str);
}

// Builds a real item by name into `out` (0x60 bytes) through the game's constructor.
static int build_item(const char* name, unsigned char* out) {
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    msvc_string s;
    size_t n = strlen(name);
    memset(&s, 0, sizeof s);
    if (n < 16) {
        memcpy(s.u.buf, name, n + 1);
        s.cap = 15;
    } else {                                   // the callee frees it: game allocator
        char* p = (char*)((fn_game_new)(base + RVA_GAME_NEW))(n + 1);
        if (!p) return 0;
        memcpy(p, name, n + 1);
        s.u.ptr = p;
        s.cap = n;
    }
    s.size = n;
    ((fn_item_ctor)(base + RVA_ITEM_CTOR))(NULL, out, &s, NULL);
    return *(unsigned long long*)(out + 0x08 + 0x10) == n;   // name made it in
}

static void stash_slot(unsigned char* cat, int slot) {
    if (g_stash_count >= (int)(sizeof g_stash / sizeof g_stash[0])) return;
    slot_stash* st = &g_stash[g_stash_count++];
    st->cat = cat; st->slot = slot;
    unsigned char* e = cat + OFF_CAT_SLOT0 + slot * EQUIP_SIZE;
    memcpy(st->saved, e, EQUIP_SIZE);          // ownership moves into the stash
    make_empty_equipment(e);
}

// House preset pool: item names in the house storage, per slot.
#define MAX_POOL 256
static const char* g_pool[NUM_SLOTS][MAX_POOL];
static char g_pool_names[NUM_SLOTS][MAX_POOL][48];
static int g_pool_count[NUM_SLOTS];

static void build_house_pool(void) {
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    memset(g_pool_count, 0, sizeof g_pool_count);
    __try {
        unsigned char* inv = *(unsigned char**)(base + RVA_INVENTORY_PTR);
        if (!inv) return;
        unsigned char* head = *(unsigned char**)(inv + 0x78 + 8);
        int guard = 0;
        for (unsigned char* node = *(unsigned char**)head; node && node != head && guard < 4096; node = *(unsigned char**)node, guard++) {
            unsigned char* eq = node + 0x18;
            char name[48];
            if (!read_std_string(eq + 8, name, sizeof name) || !name[0]) continue;
            int slot = ((fn_item_slot)(base + RVA_ITEM_SLOT))(eq);
            if (slot < 0 || slot >= NUM_SLOTS || g_pool_count[slot] >= MAX_POOL) continue;
            strcpy(g_pool_names[slot][g_pool_count[slot]], name);
            g_pool[slot][g_pool_count[slot]] = g_pool_names[slot][g_pool_count[slot]];
            g_pool_count[slot]++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("items: fault reading the house storage");
    }
    plog("items: house storage pool head/face/neck/weapon/trinket = %d/%d/%d/%d/%d",
         g_pool_count[0], g_pool_count[1], g_pool_count[2], g_pool_count[3], g_pool_count[4]);
}

// v13: the lobby's house step -- the kit each arena cat's player picked from the
// storage, in g_cat_snaps order. Built anew by name, so the storage is untouched.
static int  g_house_picked;
static char g_pick_kit[2 * MAX_TEAM][NUM_SLOTS][48];

// The storage as the lobby lists it: id, item name, slot.
static int collect_storage(lobby_item* out, int max) {
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    int n = 0;
    __try {
        unsigned char* inv = *(unsigned char**)(base + RVA_INVENTORY_PTR);
        if (!inv) return 0;
        unsigned char* head = *(unsigned char**)(inv + 0x78 + 8);
        int guard = 0;
        for (unsigned char* node = *(unsigned char**)head; node && node != head && guard < 4096 && n < max;
             node = *(unsigned char**)node, guard++) {
            unsigned char* eq = node + 0x18;
            lobby_item* it = &out[n];
            if (!read_std_string(eq + 8, it->name, sizeof it->name) || !it->name[0]) continue;
            it->slot = ((fn_item_slot)(base + RVA_ITEM_SLOT))(eq);
            if (it->slot < 0 || it->slot >= NUM_SLOTS) continue;
            it->id = *(long long*)eq;
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("items: fault reading the house storage");
    }
    return n;
}

static const char* pick_item(int slot) {
    if (g_item_preset == PRESET_HOUSE)
        return g_pool_count[slot] ? g_pool[slot][rnd(g_pool_count[slot])] : NULL;
    int n = gd_slot_count(slot);
    return n ? gd_slot_item(slot, (int)rnd(n)) : NULL;
}

static void restore_items(void);

// Called with the teams picked (g_cat_snaps holds team 1's cats, then team 2's).
static void apply_items(void) {
    static const char* const kPresetName[] = { "none", "random", "house", "balanced" };
    restore_items();
    plog("items: preset %s, %d per cat, mirror %d", kPresetName[g_item_preset], g_items_per_cat, g_items_mirror);
    if (g_item_preset == PRESET_HOUSE) build_house_pool();
    // Every arena cat starts bare: occupied slots are moved out first.
    for (int i = 0; i < g_cat_snap_count; i++)
        for (int sl = 0; sl < NUM_SLOTS; sl++) {
            unsigned char* e = g_cat_snaps[i].cat + OFF_CAT_SLOT0 + sl * EQUIP_SIZE;
            stash_slot(g_cat_snaps[i].cat, sl);
            (void)e;
        }
    if (g_force_item[0]) {
        unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
        for (int i = 0; i < g_cat_snap_count; i++) {
            unsigned char tmp[EQUIP_SIZE];
            if (!build_item(g_force_item, tmp)) { plog("items: could not build forced %s", g_force_item); break; }
            int sl = ((fn_item_slot)(base + RVA_ITEM_SLOT))(tmp);
            if (sl < 0 || sl >= NUM_SLOTS) { free_std_string(tmp + 8); free_std_string(tmp + 0x28); break; }
            unsigned char* e = g_cat_snaps[i].cat + OFF_CAT_SLOT0 + sl * EQUIP_SIZE;
            free_std_string(e + 8); free_std_string(e + 0x28);
            memcpy(e, tmp, EQUIP_SIZE);
            plog("items: cat %d slot %d <- %s (forced)", i, sl, g_force_item);
        }
    }
    if (g_item_preset == PRESET_NONE) return;
    if (g_item_preset == PRESET_HOUSE && g_house_picked) {
        int built = 0;
        for (int i = 0; i < g_cat_snap_count; i++)
            for (int sl = 0; sl < NUM_SLOTS; sl++) {
                const char* name = g_pick_kit[i][sl];
                if (!name[0]) continue;
                unsigned char* e = g_cat_snaps[i].cat + OFF_CAT_SLOT0 + sl * EQUIP_SIZE;
                // Run 57 (online): SmallMeteor rolls a random colorless ability into
                // itself when constructed (str_aux_initialize), and each machine built
                // its kits from its own house-time stream -- Roll on one, Donate on the
                // other. Online, both TLS streams are seeded from the START seed and
                // the slot for the constructor, then put back.
                unsigned char saved[64], *tls = NULL;
                if (g_arena_online && g_online_seed_set) {
                    unsigned char** blocks = (unsigned char**)__readgsqword(0x58);
                    tls = blocks ? blocks[0] : NULL;
                    if (tls) {
                        memcpy(saved, tls + 0x178, 64);   // sim 0x178 and presentation 0x198
                        unsigned long long x = fnv64(g_online_seed, 32, 0xCBF29CE484222325ULL) ^
                                               ((unsigned long long)(i * NUM_SLOTS + sl + 1) * 0x9E3779B97F4A7C15ULL);
                        for (int w = 0; w < 8; w++) {     // splitmix64
                            unsigned long long z = (x += 0x9E3779B97F4A7C15ULL);
                            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                            ((unsigned long long*)(tls + 0x178))[w] = z ^ (z >> 31);
                        }
                    }
                }
                int ok = build_item(name, e);
                if (tls) memcpy(tls + 0x178, saved, 64);
                if (ok) { built++; plog("items: cat %d slot %d <- %s (picked)", i, sl, name); }
                else { plog("items: could not build %s", name); make_empty_equipment(e); }
            }
        plog("items: %d picked item(s) built", built);
        return;
    }
    // Rolls for team 1's cats; with Mirror, team 2's cat i gets cat i's kit.
    const char* kit[MAX_TEAM][NUM_SLOTS];
    memset(kit, 0, sizeof kit);
    for (int t = 0; t < 2; t++) {
        for (int i = 0; i < g_team_size[t]; i++) {
            if (t == 1 && g_items_mirror && i < g_team_size[0]) continue;   // reuse below
            int order[NUM_SLOTS] = { 0, 1, 2, 3, 4 };
            for (int k = NUM_SLOTS - 1; k > 0; k--) { int j = rnd(k + 1); int tmp = order[k]; order[k] = order[j]; order[j] = tmp; }
            for (int k = 0; k < NUM_SLOTS; k++) kit[i][k] = NULL;
            for (int k = 0; k < g_items_per_cat && k < NUM_SLOTS; k++) kit[i][order[k]] = pick_item(order[k]);
            // Team 1 cat i keeps its kit for the mirror; give it now.
            unsigned char* cat = g_cat_snaps[(t == 0 ? 0 : g_team_size[0]) + i].cat;
            for (int sl = 0; sl < NUM_SLOTS; sl++) {
                if (!kit[i][sl]) continue;
                unsigned char* e = cat + OFF_CAT_SLOT0 + sl * EQUIP_SIZE;
                if (build_item(kit[i][sl], e)) plog("items: team %d cat %d slot %d <- %s", t + 1, i, sl, kit[i][sl]);
                else { plog("items: could not build %s", kit[i][sl]); make_empty_equipment(e); }
            }
        }
        if (t == 0 && g_items_mirror) {
            for (int i = 0; i < g_team_size[1] && i < g_team_size[0]; i++) {
                unsigned char* cat = g_cat_snaps[g_team_size[0] + i].cat;
                for (int sl = 0; sl < NUM_SLOTS; sl++) {
                    if (!kit[i][sl]) continue;
                    unsigned char* e = cat + OFF_CAT_SLOT0 + sl * EQUIP_SIZE;
                    if (build_item(kit[i][sl], e)) plog("items: team 2 cat %d slot %d <- %s (mirror)", i, sl, kit[i][sl]);
                    else make_empty_equipment(e);
                }
            }
        }
    }
}

static void restore_items(void) {
    if (!g_stash_count) return;
    __try {
        for (int i = g_stash_count - 1; i >= 0; i--) {
            unsigned char* e = g_stash[i].cat + OFF_CAT_SLOT0 + g_stash[i].slot * EQUIP_SIZE;
            free_std_string(e + 0x08);          // whatever the arena put there
            free_std_string(e + 0x28);
            memcpy(e, g_stash[i].saved, EQUIP_SIZE);
        }
        plog("items: %d slot(s) restored", g_stash_count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("items: fault restoring slots");
    }
    g_stash_count = 0;
}

// ---------------------------------------------------------------------------
// v18: the bird's item goes straight onto the cat that earned it.
//
// BirdRewards (FUN_1407E1E90) hands `ally_rewards` -- FindItemFromPool -- to the
// killer when it is player side or a cat (+0x489 with CatData), else to the first
// such ally. FindItemFromPool's handler FUN_140304360(?, Character* recipient (RDX),
// source, ?, GonObject* pool) would return at once in an arena (it bails when
// MewDirector+0x70C is set, which the house-battle start sets) and would otherwise
// put the item into the house inventory. So during an arena it is replaced: a
// random regular item (gamedata.cpp: the slot pool read from the player's game) is built and equipped the way StealEquipment
// (FUN_14030A050) equips a stolen item -- FUN_1400CA2E0(CatData*, Equipment*, 4, 0)
// then FUN_1400FE980(Character*) to rebuild the fighter. The target slot is emptied
// first: FUN_1400CA2E0 sends the old item to the inventory (FUN_1402E13F0), and
// whatever an arena cat wears is the arena's own (the originals are in g_stash,
// put back on the way home).
#define RVA_FIND_ITEM_FROM_POOL 0x304360
#define RVA_EQUIP_ITEM          0x0CA2E0
#define RVA_REFRESH_FIGHTER     0x0FE980
static const unsigned char kFindItemEntry[15] = {
    0x48,0x8B,0xC4, 0x4C,0x89,0x40,0x18, 0x48,0x89,0x50,0x10, 0x48,0x89,0x48,0x08 };
typedef void (__fastcall *fn_find_item)(void*, void*, void*, void*, void*);
typedef char (__fastcall *fn_equip_item)(void*, void*, int, int);
typedef void (__fastcall *fn_refresh_fighter)(void*);
static fn_find_item g_orig_find_item;

static void __fastcall hook_find_item(void* a, void* recipient, void* src, void* d, void* pool) {
    if (!g_arena_active) { g_orig_find_item(a, recipient, src, d, pool); return; }
    __try {
        unsigned char* ch = (unsigned char*)recipient;
        unsigned char* cat = ch ? *(unsigned char**)(ch + OFF_CATDATA) : NULL;
        if (!cat || !team_of_cat(cat)) { plog("birds: reward for %p, not an arena cat -- dropped", recipient); return; }
        unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
        int slot = rnd_fight(NUM_SLOTS);
        // Any regular item (gamedata.cpp: the slot pool read from the player's game), whatever the item preset.
        int pool = gd_slot_count(slot);
        const char* name = pool ? gd_slot_item(slot, (int)rnd_fight(pool)) : NULL;
        unsigned char item[EQUIP_SIZE];
        if (!name || !build_item(name, item)) { plog("birds: could not build an item"); return; }
        int sl = ((fn_item_slot)(base + RVA_ITEM_SLOT))(item);
        if (sl < 0 || sl >= NUM_SLOTS) { free_std_string(item + 8); free_std_string(item + 0x28); return; }
        unsigned char* e = cat + OFF_CAT_SLOT0 + sl * EQUIP_SIZE;
        free_std_string(e + 0x08);
        free_std_string(e + 0x28);
        make_empty_equipment(e);
        ((fn_equip_item)(base + RVA_EQUIP_ITEM))(cat, item, 4, 0);
        free_std_string(item + 0x08);            // the callee copied it
        free_std_string(item + 0x28);
        ((fn_refresh_fighter)(base + RVA_REFRESH_FIGHTER))(ch);
        plog("birds: team %d cat key %lld slot %d <- %s", team_of_cat(cat),
             *(long long*)(cat + OFF_CAT_SQL_KEY), sl, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("birds: fault giving the reward");
    }
}

// ---------------------------------------------------------------------------
// v19: bird waves during the fight (owner, run 42: "1-2 birds every few turns").
//
// Rounds: Character::RoundUpkeep (FUN_14010C780, named by its lambdas' RTTI) runs for
// the characters once per round, from the battle state update FUN_1408E2A30. A call
// more than a second after the previous one starts a new round. Spawning is done on
// the next frame from the swap detour, not inside that per-character loop.
//
// Spawning mid-fight the way SpawnCoinAnywhere (FUN_14061A9D0) does it:
//   level = FUN_140049630(Character*)            -- the tactics level
//   FUN_1408AC5B0(level, &tile, 0)              -- a random free tile, x | y << 32
//   ent = FUN_1407C3700(source or NULL, ctx or NULL, std::string name /*by value*/,
//                       tile, double pos[3] = {x + .5, 0, y + .5}, 3)
//   FUN_1401230A0(ent)                          -- its Character
#define RVA_ROUND_UPKEEP 0x10C780
#define RVA_LEVEL_OF     0x049630
#define RVA_FREE_TILE    0x8AC5B0
#define RVA_SPAWN_UNIT   0x7C3700
// Run 44: the wave faulted. With a NULL source FUN_1407C3700 spawns into the scene at
// 0x1413C18F0, a "current scene" global the scene update sets and clears around its
// own work (FUN_14096A6F0 & co.); from the swap detour it is not set. So it is set to
// the battle scene (Character+0x20) for the spawn and put back afterwards.
#define RVA_CURRENT_SCENE 0x13C18F0
static const unsigned char kRoundUpkeepEntry[15] = {
    0x48,0x89,0x5C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x48,0x89,0x7C,0x24,0x20 };
typedef void  (__fastcall *fn_round_upkeep)(void*);
typedef void* (__fastcall *fn_level_of)(void*);
typedef long long* (__fastcall *fn_free_tile)(void*, long long*, long long);
typedef void* (__fastcall *fn_spawn_unit)(void*, void*, msvc_string*, long long, double*, int);
static fn_round_upkeep g_orig_round_upkeep;
static int  g_bird_every;             // a wave every N rounds, 0 = off
static int  g_bird_wave = 1;          // birds per wave
static int  g_round;                  // rounds seen in this fight
static unsigned long long g_last_upkeep_ms;
static volatile LONG g_wave_pending;
static void bird_wave_spawn(void);

static void __fastcall hook_round_upkeep(void* ch) {
    g_orig_round_upkeep(ch);
    if (!g_arena_active) return;
    if (g_arena_online) {
        // Online the round and the wave must be the same instant of the simulation
        // on both machines, so neither comes from the wall clock or a frame tick: a
        // round starts when RoundUpkeep reaches a character it already reached this
        // round (the game calls it per character, in the same order on both), and
        // the wave spawns right here, inside the simulation step. The free tile comes
        // from FUN_1408AC5B0, which draws on the sim stream, and the kind from the
        // fight's seeded generator.
        int seen = 0;
        for (int i = 0; i < g_upkept_n; i++) if (g_upkept[i] == ch) { seen = 1; break; }
        if (seen) g_upkept_n = 0;
        if (g_upkept_n == 0) {
            g_round++;
            plog("round: %d (online)", g_round);
            if (g_bird_every > 0 && g_round > 1 && (g_round - 1) % g_bird_every == 0) bird_wave_spawn();
        }
        if (g_upkept_n < (int)(sizeof g_upkept / sizeof g_upkept[0])) g_upkept[g_upkept_n++] = ch;
        return;
    }
    unsigned long long now = GetTickCount64();
    if (now - g_last_upkeep_ms > 1000) {
        g_round++;
        plog("round: %d", g_round);
        if (g_bird_every > 0 && g_round % g_bird_every == 0) InterlockedExchange(&g_wave_pending, 1);
    }
    g_last_upkeep_ms = now;
}

static void bird_wave_tick(void) {
    if (!InterlockedExchange(&g_wave_pending, 0) || !g_arena_active) return;
    bird_wave_spawn();
}

static void bird_wave_spawn(void) {
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    void** cur_scene = (void**)(base + RVA_CURRENT_SCENE);
    void* saved_scene = *cur_scene;
    __try {
        void* anchor = NULL;           // any living arena cat: gives the level
        for (int i = 0; i < g_arena_count && !anchor; i++)
            if (g_arena_cats[i] && !((unsigned char*)g_arena_cats[i])[0x4C2]) anchor = g_arena_cats[i];
        if (!anchor) { plog("birds: wave, but no living cat to anchor on"); return; }
        unsigned char* level = (unsigned char*)((fn_level_of)(base + RVA_LEVEL_OF))(anchor);
        if (!level) { plog("birds: wave, no level"); return; }
        void* scene = *(void**)((unsigned char*)anchor + 0x20);
        plog("birds: level %p, battle scene %p, current scene was %p", level, scene, saved_scene);
        if (!scene) return;
        *cur_scene = scene;
        int alive = 0;
        for (int i = 0; i < MAX_BIRDS; i++) {
            unsigned char* b = (unsigned char*)g_birds[i];
            // Run 45 (live memory): birds have +0x365 = 0 (a cats' flag), so only the
            // "dead" byte counts here.
            if (b && !b[0x4C2]) alive++;
        }
        int want = g_bird_wave - alive;   // owner, run 43: the number is a cap on living birds
        plog("birds: round %d, %d alive, cap %d -> %d new", g_round, alive, g_bird_wave, want > 0 ? want : 0);
        int made = 0;
        for (int k = 0; k < want; k++) {
            long long tile = -1;
            ((fn_free_tile)(base + RVA_FREE_TILE))(level, &tile, 0);
            int x = (int)(tile & 0xFFFFFFFF), y = (int)(tile >> 32);
            if (x < 0 || y < 0 || (unsigned)x >= *(unsigned*)(level + 0xB8) || (unsigned)y >= *(unsigned*)(level + 0xBC)) {
                plog("birds: no free tile");
                break;
            }
            static const char* kNames[3] = { "PvpBirdSmall", "PvpBirdMed", "PvpBirdLarge" };
            msvc_string name;
            make_sso(&name, kNames[g_arena_online ? bird_kind_pick_fight() : bird_kind_pick()]);   // the callee destroys it
            double pos[3] = { x + 0.5, 0.0, y + 0.5 };
            void* ent = ((fn_spawn_unit)(base + RVA_SPAWN_UNIT))(NULL, NULL, &name, tile, pos, 3);
            plog("birds: wave bird at (%d,%d) -> %p", x, y, ent);
            if (ent) made++;
        }
        *cur_scene = saved_scene;
        plog("birds: round %d wave, %d bird(s)", g_round, made);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *cur_scene = saved_scene;
        plog("birds: fault spawning a wave");
    }
}

static void read_item_config(HMODULE self) {
    char path[MAX_PATH], v[16];
    pvp_ini_path(self, path);
    GetPrivateProfileStringA("items", "Preset", "none", v, sizeof v, path);
    // Owner, run 26: two random flavours -- "random" (every cat its own roll) and
    // "balanced" (team 2's cat N gets team 1's cat N's kit). Mirror now only
    // matters for "house".
    g_item_preset = !_stricmp(v, "random") ? PRESET_RANDOM : !_stricmp(v, "balanced") ? PRESET_BALANCED
                  : !_stricmp(v, "house") ? PRESET_HOUSE : PRESET_NONE;
    g_items_per_cat = GetPrivateProfileIntA("items", "PerCat", 2, path);
    if (g_items_per_cat < 0) g_items_per_cat = 0;
    if (g_items_per_cat > NUM_SLOTS) g_items_per_cat = NUM_SLOTS;
    g_items_mirror = GetPrivateProfileIntA("items", "Mirror", 1, path);
    GetPrivateProfileStringA("items", "ForceItem", "", g_force_item, sizeof g_force_item, path);
    if (g_item_preset == PRESET_RANDOM) g_items_mirror = 0;
    if (g_item_preset == PRESET_BALANCED) g_items_mirror = 1;
}

// v12: starting money. Level::init writes 20 into the adventure inventory's coins
// (+0x148) in house-battle mode; when the lobby asks for a sum it is written
// after Level::init instead. The counters are restored on the way home as before.
static int g_money_on, g_money;

static void start_arena_with_teams(unsigned char* house, unsigned char* md) {
    if (!install_run_list(md)) return;
    apply_items();
    save_rng();
    start_fight(house, 1);
}

// F9: quick start with the ini's settings and rooms.
static void start_arena(void) {
    unsigned char* house = find_house_scene();
    if (!house) { plog("F9: no live House scene, ignored"); return; }
    unsigned char* md = *(unsigned char**)((unsigned char*)GetModuleHandleA(NULL) + RVA_MEWDIRECTOR_PTR);
    if (!md || !pick_teams(house)) return;
    g_house_picked = 0;
    g_level_fixed[0] = 0; g_online_seed_set = 0; g_arena_online = 0;   // hotseat
    start_arena_with_teams(house, md);
}

// ---------------------------------------------------------------------------
// v12: the lobby (lobby.cpp). The АРЕНА button opens it with every living house
// cat (name: CatData+0x18, std::wstring) and the two default rooms preassigned;
// СТАРТ hands back the chosen cats and settings.

static int read_std_wstring_utf8(const unsigned char* p, char* out, int outsz) {
    unsigned long long size = *(unsigned long long*)(p + 0x10), cap = *(unsigned long long*)(p + 0x18);
    if (size > 256 || cap < size) return 0;
    const wchar_t* src = cap > 7 ? *(const wchar_t**)p : (const wchar_t*)p;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, (int)size, out, outsz - 1, NULL, NULL);
    out[n > 0 ? n : 0] = 0;
    return n > 0;
}

static char g_player_name[2][32] = { "Игрок 1", "Игрок 2" };

// The arena's language follows the game's (settings.txt "current_language", the
// newest one under %APPDATA%\Glaiel Games\Mewgenics): Russian when the game is in
// Russian, English otherwise. pvp.ini [probe] Language=ru|en overrides it.
static int g_en;
#define TRW(ru, en) (g_en ? (en) : (ru))

static void detect_language(HMODULE self) {
    char ini[MAX_PATH], forced[8] = "";
    pvp_ini_path(self, ini);
    GetPrivateProfileStringA("probe", "Language", "", forced, sizeof forced, ini);
    if (forced[0]) { g_en = _stricmp(forced, "ru") != 0; return; }
    char base[MAX_PATH], pat[MAX_PATH], best[MAX_PATH] = "";
    FILETIME best_t = { 0, 0 };
    if (!GetEnvironmentVariableA("APPDATA", base, MAX_PATH)) { g_en = 1; return; }
    _snprintf(pat, MAX_PATH, "%s\\Glaiel Games\\Mewgenics\\*", base);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            char f[MAX_PATH];
            WIN32_FILE_ATTRIBUTE_DATA a;
            _snprintf(f, MAX_PATH, "%s\\Glaiel Games\\Mewgenics\\%s\\settings.txt", base, fd.cFileName);
            if (GetFileAttributesExA(f, GetFileExInfoStandard, &a) && CompareFileTime(&a.ftLastWriteTime, &best_t) > 0) {
                best_t = a.ftLastWriteTime;
                strcpy(best, f);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    g_en = 1;
    FILE* f = best[0] ? fopen(best, "rb") : NULL;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "current_language ", 17)) { g_en = strncmp(line + 17, "ru", 2) != 0; break; }
        fclose(f);
    }
    if (g_en) { strcpy(g_player_name[0], "Player 1"); strcpy(g_player_name[1], "Player 2"); }
    plog("language: %s (from %s)", g_en ? "English" : "Russian", best[0] ? best : "default");
}

static void open_lobby(void) {
    unsigned char* house = find_house_scene();
    if (!house) { plog("lobby: no live House scene"); return; }
    room_cats rooms[MAX_ROOMS];
    int nrooms = collect_rooms(house, rooms);
    // Default teams: the same two rooms the F9 path would take.
    int pick[2] = { -1, -1 };
    for (int t = 0; t < 2; t++) {
        for (int r = 0; r < nrooms && g_team_room[t][0]; r++)
            if (strcmp(rooms[r].name, g_team_room[t]) == 0 && rooms[r].n > 0) pick[t] = r;
        for (int r = 0; r < nrooms && pick[t] < 0; r++)
            if (rooms[r].n > 0 && r != pick[1 - t]) pick[t] = r;
    }
    static lobby_cat cats[LOBBY_MAX_CATS];
    int n = 0, taken[2] = { 0, 0 };
    int team_max = g_team_max > 4 ? 4 : g_team_max;
    __try {
        for (int r = 0; r < nrooms; r++)
            for (int i = 0; i < rooms[r].n && n < LOBBY_MAX_CATS; i++) {
                lobby_cat* c = &cats[n++];
                memset(c, 0, sizeof *c);
                c->cat = rooms[r].cats[i];
                c->key = *(long long*)(rooms[r].cats[i] + OFF_CAT_SQL_KEY);
                if (!read_std_wstring_utf8(rooms[r].cats[i] + 0x18, c->name, sizeof c->name))
                    sprintf(c->name, g_en ? "cat %lld" : "кот %lld", c->key);
                strncpy(c->room, rooms[r].name, sizeof c->room - 1);
                // v20: face parts (combat_roster's offsets: BodyParts @+0x60, texture +0x18,
                // palette +0x1C, part descriptors @+0x2C stride 0x54, index +4).
                {
                    static const int kPart[5] = { 1, 11, 7, 9, 13 };   // head ear eye brow mouth
                    unsigned char* cd = rooms[r].cats[i];
                    for (int k = 0; k < 5; k++) c->look[k] = *(int*)(cd + 0x60 + 0x2C + kPart[k] * 0x54 + 4);
                    c->look[5] = *(int*)(cd + 0x60 + 0x18);
                    c->look[6] = *(int*)(cd + 0x60 + 0x1C);
                    // v21: battle abilities and passives (mewgenics-cat-bridge glaiel_cat.hpp:
                    // actives_accessible[4] @+0x810, passive_0 @+0x910, passive_1 @+0x938).
                    for (int k = 0; k < 4; k++) read_std_string(cd + 0x810 + k * 0x20, c->abil[k], sizeof c->abil[k]);
                    read_std_string(cd + 0x910, c->passive[0], sizeof c->passive[0]);
                    read_std_string(cd + 0x938, c->passive[1], sizeof c->passive[1]);
                }
                // List details (owner, run 29). Offsets: mewgenics-cat-bridge CatData;
                // current day at MewDirector+0x580 (its current_day()).
                unsigned char* cd = rooms[r].cats[i];
                unsigned char* mdp = *(unsigned char**)((unsigned char*)GetModuleHandleA(NULL) + RVA_MEWDIRECTOR_PTR);
                c->sex = *(int*)(cd + 0x58);
                c->level = *(int*)(cd + 0xC30);
                c->age_days = mdp ? (int)(*(long long*)(mdp + 0x580) - *(long long*)(cd + 0xC38)) : 0;
                read_std_string(cd + 0xC10, c->cls, sizeof c->cls);
                for (int k = 0; k < 7; k++)
                    c->stats[k] = *(int*)(cd + 0x6F0 + 4 * k) + *(int*)(cd + 0x70C + 4 * k) + *(int*)(cd + 0x728 + 4 * k);
                for (int t = 0; t < 2; t++)
                    if (r == pick[t] && taken[t] < team_max) { c->team = t + 1; taken[t]++; }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("lobby: fault reading the cats");
        return;
    }
    lobby_settings s;
    memset(&s, 0, sizeof s);
    memcpy(s.player_name, g_player_name, sizeof s.player_name);
    s.item_preset = g_item_preset;
    s.items_per_cat = g_items_per_cat ? g_items_per_cat : 2;
    s.house_mirror = g_items_mirror;
    s.money_on = g_money_on;
    s.money = g_money ? g_money : 20;
    s.team_max = team_max;
    s.cover_count = g_cover_count;
    s.cover_kind = g_cover_kind;
    s.loot_count = g_loot_count;
    s.loot_mask = g_loot_mask;
    s.mob_count = g_mob_count;
    s.mob_kind = g_mob_kind;
    s.mob_mode = g_mob_mode;
    s.mob_mad = g_mob_mad;
    s.bird_count = g_bird_count;
    s.bird_mask = g_bird_mask;
    s.bird_every = g_bird_every;
    s.bird_wave = g_bird_wave;
    static lobby_item store[LOBBY_MAX_ITEMS];
    int nstore = collect_storage(store, LOBBY_MAX_ITEMS);
    plog("lobby: %d item(s) in the storage", nstore);
    lobby_set_storage(store, nstore);
    if (net_open_lobby(cats, n, &s)) return;
    lobby_open(cats, n, &s);
}

static void lobby_tick(void) {
    lobby_on_swap();
    if (net_lobby_tick()) return;
    static lobby_cat chosen[LOBBY_MAX_CATS];
    int n = 0;
    lobby_settings s;
    if (!lobby_take_start(chosen, &n, &s) || g_arena_active) return;
    unsigned char* house = find_house_scene();
    unsigned char* md = *(unsigned char**)((unsigned char*)GetModuleHandleA(NULL) + RVA_MEWDIRECTOR_PTR);
    if (!house || !md) { plog("lobby: start, but no house / MewDirector"); return; }
    memcpy(g_player_name, s.player_name, sizeof g_player_name);
    g_item_preset = s.item_preset;
    g_items_per_cat = s.items_per_cat;
    g_items_mirror = s.item_preset == PRESET_RANDOM ? 0 : s.item_preset == PRESET_BALANCED ? 1 : s.house_mirror;
    g_money_on = s.money_on;
    g_money = s.money;
    g_cover_count = s.cover_count;
    g_cover_kind = s.cover_kind;
    g_loot_count = s.loot_count;
    g_loot_mask = s.loot_mask;
    g_mob_count = s.mob_count;
    g_mob_kind = s.mob_kind;
    g_mob_mode = s.mob_mode;
    g_mob_mad = s.mob_mad;
    g_bird_count = s.bird_count;
    g_bird_mask = s.bird_mask;
    g_bird_every = s.bird_every;
    g_bird_wave = s.bird_wave;
    g_house_picked = s.item_preset == PRESET_HOUSE;
    g_level_fixed[0] = 0; g_online_seed_set = 0; g_arena_online = 0;   // hotseat
    g_cat_snap_count = 0;
    g_team_size[0] = g_team_size[1] = 0;
    // mgmp build: the fight uses CLONES of the chosen cats (pvp_clone.h) -- built
    // from the bytes SerializeCatData writes for the real cat, under keys no save
    // can hold. The real cats never enter the battle. Clones are kept between
    // fights and re-read here, so a stale key from the last fight is harmless.
    pvp_clone_clear();
    int clone_slot = 0;
    for (int t = 1; t <= 2; t++)          // team 1's cats first, then team 2's
        for (int i = 0; i < n; i++)
            if (chosen[i].team == t && g_team_size[t - 1] < MAX_TEAM) {
                memcpy(g_pick_kit[g_cat_snap_count], chosen[i].kit, sizeof g_pick_kit[0]);
                unsigned char* fighter = (unsigned char*)chosen[i].cat;
                uint8_t* bytes = NULL;
                uint32_t len = pvp_clone_serialize(chosen[i].cat, &bytes);
                void* clone = len ? pvp_clone_from_bytes(clone_slot, bytes, len) : NULL;
                free(bytes);
                if (clone) { fighter = (unsigned char*)clone; clone_slot++; }
                else plog("lobby: !! no clone for %s -- the real cat fights", chosen[i].name);
                plog("lobby: %s clone slot %d (%u bytes) -> %p", chosen[i].name, clone_slot - 1, len, clone);
                snapshot_cat(fighter, t - 1, g_team_size[t - 1]);
                g_team_size[t - 1]++;
                plog("lobby: %s <- %s (key %lld, %s)", g_player_name[t - 1], chosen[i].name, chosen[i].key, chosen[i].room);
            }
    plog("lobby: start, %d vs %d, items %d x%d, money %s %d", g_team_size[0], g_team_size[1],
         g_item_preset, g_items_per_cat, g_money_on ? "on" : "off", g_money);
    start_arena_with_teams(house, md);
}

// [teams] Team1Room / Team2Room = room names as the log prints them
// ("Floor1_Small", "Attic", ...); empty = first rooms with cats. TeamSize caps
// each side (default 4, at most 8; the arena has 4 spawns per side).
static void read_team_config(HMODULE self) {
    char path[MAX_PATH];
    pvp_ini_path(self, path);
    GetPrivateProfileStringA("teams", "Team1Room", "", g_team_room[0], sizeof g_team_room[0], path);
    GetPrivateProfileStringA("teams", "Team2Room", "", g_team_room[1], sizeof g_team_room[1], path);
    g_team_max = GetPrivateProfileIntA("teams", "TeamSize", 4, path);
    if (g_team_max < 1) g_team_max = 1;
    if (g_team_max > MAX_TEAM) g_team_max = MAX_TEAM;
}

// ---------------------------------------------------------------------------
// v4: the referee. The game cannot end this fight (house-battle mode, no boss
// controller), so the probe does: once every cat of one side is dead
// (Character+0x4C2), the other side wins, and after a pause for the death
// animation both go back to the house -- with nothing written to the save.
//
// Going back is the scene part of MewDirector's run-end (FUN_1403B4750) and
// nothing else: Director::DestroyScene("Battle"), Director::CreateScene("House"),
// Scene::CreateEntity, then the House builder FUN_1403C3EE0(scene, entity,
// &night). Run-end also deletes the adventure keys from the save and
// MewDirector::SaveGame (FUN_1403BDCF0) commits it; neither is called here.
// `night` is whatever the game passed the last time it built the house itself.

#define RVA_DESTROY_SCENE 0x9D4E00   // Director::DestroyScene(std::string) by value
#define RVA_CREATE_SCENE  0x9D4950   // Director::CreateScene(std::string) by value
#define RVA_CREATE_ENTITY 0x96B3E0   // Scene::CreateEntity()
#define RVA_BUILD_HOUSE   0x3C3EE0   // (Scene*, Entity*, bool* night)
#define OFF_CHAR_DEAD     0x4C2
#define OFF_CHAR_ACTIVE   0x365
#define OFF_SCENE_CLOSING 0x4B0

typedef void  (__fastcall *fn_destroy_scene)(void*, msvc_string*);
typedef void* (__fastcall *fn_create_scene)(void*, msvc_string*);
typedef void* (__fastcall *fn_create_entity)(void*);
typedef void* (__fastcall *fn_build_house)(void*, void*, unsigned char*);
static fn_build_house g_orig_build_house;
static unsigned char  g_house_night;
static int            g_house_night_seen;
static ULONGLONG      g_end_at;

static const unsigned char kBuildHouseEntry[16] = {
    0x40,0x53, 0x55, 0x41,0x56, 0x48,0x83,0xEC,0x20, 0x80,0xB9,0xB0,0x04,0x00,0x00,0x00 };

static void* __fastcall hook_build_house(void* scene, void* entity, unsigned char* night) {
    if (night) { g_house_night = *night; g_house_night_seen = 1; }
    plog("House built by the game: scene=%p night=%d", scene, night ? *night : -1);
    return g_orig_build_house(scene, entity, night);
}

// Declared here too (tentative definition) because return_to_house forgets them.
static unsigned char *g_btn_resume, *g_btn_giveup, *g_btn_menu, *g_btn_quit;

static void return_to_house(void) {
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    unsigned char* md = *(unsigned char**)(base + RVA_MEWDIRECTOR_PTR);
    void* director = *(void**)(md + OFF_DIR_DIRECTOR);
    restore_director(md);
    restore_run_list(md);
    restore_cats();
    restore_items();
    restore_rng();
    g_btn_resume = g_btn_giveup = g_btn_menu = g_btn_quit = NULL;

    msvc_string battle, house;
    make_sso(&battle, "Battle");
    ((fn_destroy_scene)(base + RVA_DESTROY_SCENE))(director, &battle);
    make_sso(&house, "House");
    unsigned char* scene = (unsigned char*)((fn_create_scene)(base + RVA_CREATE_SCENE))(director, &house);
    unsigned char night = g_house_night_seen ? g_house_night : 0;
    if (scene && !scene[OFF_SCENE_CLOSING]) {
        void* ent = ((fn_create_entity)(base + RVA_CREATE_ENTITY))(scene);
        ((fn_build_house)(base + RVA_BUILD_HOUSE))(scene, ent, &night);
    }
    plog("referee: back to the house (new House scene %p, night=%d)", scene, night);
}

// ---------------------------------------------------------------------------
// v5: the winner banner. The round banner is a CombatMessagePopup component:
// NextTurn creates an entity in the battle scene and calls
//     FUN_1408E93C0(Scene*, Entity*, <unused>, std::wstring* text)
// which MOVES the text out of the wstring and plays it in the clip
// "CombatMessage_RoundBegin". (CombatMessage_Victory itself is a sound event,
// FUN_14095B770, not a banner; the real victory screen is the rewards screen.)
//
// The game frees the moved buffer, so a heap buffer must come from the game's own
// operator new (FUN_140052A40), never from this DLL's CRT.

#define RVA_SHOW_POPUP  0x8E93C0

typedef struct { union { wchar_t buf[8]; wchar_t* ptr; } u; unsigned long long size, cap; } msvc_wstring;
typedef void* (__fastcall *fn_show_popup)(void*, void*, void*, msvc_wstring*);

// Returns the popup's entity (NULL on failure).
static unsigned char* show_banner(const wchar_t* text) {
    unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
    unsigned char* scene = find_scene("Battle");
    if (!scene || scene[OFF_SCENE_CLOSING]) { plog("banner: no live Battle scene"); return NULL; }
    size_t n = wcslen(text);
    msvc_wstring ws;
    memset(&ws, 0, sizeof ws);
    if (n < 8) {
        memcpy(ws.u.buf, text, (n + 1) * sizeof(wchar_t));
        ws.cap = 7;
    } else {
        unsigned long long cap = n | 7;   // what MSVC's growth would pick, >= n
        wchar_t* p = (wchar_t*)((fn_game_new)(base + RVA_GAME_NEW))((cap + 1) * sizeof(wchar_t));
        if (!p) { plog("banner: allocation failed"); return NULL; }
        memcpy(p, text, (n + 1) * sizeof(wchar_t));
        ws.u.ptr = p;
        ws.cap = cap;
    }
    ws.size = n;
    unsigned char* ent = (unsigned char*)((fn_create_entity)(base + RVA_CREATE_ENTITY))(scene);
    void* popup = ((fn_show_popup)(base + RVA_SHOW_POPUP))(scene, ent, NULL, &ws);
    plog("banner: popup %p, entity %p, on Battle scene %p", popup, ent, scene);
    return ent;
}

// After the winner banner the fight waits for the players: Enter goes back to the
// house, R starts a rematch on a fresh arena. Keyboard only for now; real buttons
// need the game's SWF Button plumbing.
//
// Keeping the banner up. The popup's entity also gets an AnimationDeleter
// (FUN_14005A6C0, from FUN_140059E80) that deletes the entity once the clip has
// played out. Every component's time step is
//     Entity+0x10 (double, 1.0) * Component+0x30 (double, 1.0) * scene dt
// (read off Level::update), so setting the banner entity's +0x10 to 0 once the
// text is fully in (g_freeze_ms) stops both the clip and its deleter. The entity
// goes away with the Battle scene on either choice.
#define OFF_ENTITY_TIMESCALE 0x10
static int g_key_enter_down, g_key_r_down;
static unsigned char* g_banner_ent;
static ULONGLONG g_freeze_at;

static int key_pressed(int vk, int* was_down) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int edge = down && !*was_down;
    *was_down = down;
    return edge && game_has_focus();
}

static void do_rematch(void);

static void end_choice_tick(void) {
    if (g_banner_ent && g_freeze_at && GetTickCount64() >= g_freeze_at) {
        __try {
            *(double*)(g_banner_ent + OFF_ENTITY_TIMESCALE) = 0.0;
            plog("banner: frozen after %d ms", g_freeze_ms);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            plog("banner: entity gone before freeze");
        }
        g_freeze_at = 0;
    }
    int enter = key_pressed(VK_RETURN, &g_key_enter_down);
    int rematch = key_pressed('R', &g_key_r_down);
    if (!enter && !rematch) return;
    if (net_end_choice(rematch)) return;   // online: both machines go together
    g_end_at = 0;
    g_banner_ent = NULL;
    g_freeze_at = 0;
    if (rematch) {
        unsigned char* battle = find_scene("Battle");
        if (battle) { do_rematch(); return; }
        plog("referee: rematch asked but no Battle scene, going home instead");
    }
    InterlockedExchange(&g_arena_active, 0);
    g_arena_count = 0;
    return_to_house();
}

// ---------------------------------------------------------------------------
// v8.5: "Surrender" in the pause menu during an arena. PauseMenu::TryAbandonRun's
// confirm (FUN_14029AE10) calls MewDirector::Transition(md, "defeat", AbandonRun
// lambda) (FUN_1403C0210) and then closes the menu itself. The defeat path builds
// a run summary from the cat list at MewDirector+0x5C8/+0x5CC/+0x5D0 and reads its
// first entry unchecked -- empty for an arena from the house, so run 12 crashed
// (null read at Mewgenics.exe+0x8F8C6) before anything was saved.
//
// So while an arena is on, a "defeat" transition is dropped and the arena exits
// to the house at the end of the frame instead (not inside the menu callback,
// which runs while the director walks its scenes). The dropped arguments need no
// cleanup: the name "defeat" lives in the string's small buffer and the
// AbandonRun lambda only captures a pointer.
#define RVA_TRANSITION 0x3C0210
static const unsigned char kTransitionEntry[16] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0xF2,0x0F,0x11,0x58,0x20, 0x4C,0x89,0x40,0x18 };
typedef void (__fastcall *fn_transition)(void*, msvc_string*, void*, double);
static fn_transition g_orig_transition;
static volatile LONG g_pending_exit;

static void __fastcall hook_transition(void* md, msvc_string* name, void* fn, double t) {
    if (g_arena_active && name && name->size == 6 && name->cap == 15 &&
        memcmp(name->u.buf, "defeat", 6) == 0) {
        plog("arena: 'defeat' transition (surrender) dropped, leaving to the house");
        InterlockedExchange(&g_pending_exit, 1);
        return;
    }
    g_orig_transition(md, name, fn, t);
}

static void pending_exit_tick(void) {
    if (!InterlockedExchange(&g_pending_exit, 0) || !g_arena_active) return;
    g_end_at = 0;
    g_banner_ent = NULL;
    g_freeze_at = 0;
    InterlockedExchange(&g_arena_active, 0);
    g_arena_count = 0;
    return_to_house();
}

// Same teams, fresh cats: undo the last fight's injuries / campaign HP first.
// From R after a win, or from the pause menu's РЕВАНШ at any time.
static void do_rematch(void) {
    unsigned char* battle = find_scene("Battle");
    if (!battle) { plog("referee: rematch asked but no Battle scene"); return; }
    plog("referee: rematch");
    g_end_at = 0;
    g_banner_ent = NULL;
    g_freeze_at = 0;
    restore_cats();
    apply_items();
    start_fight(battle, 0);
}

static void referee_tick(void) {
    if (!g_arena_active || g_arena_count < 2) return;
    if (g_end_at) { end_choice_tick(); return; }
    int alive[3] = { 0, 0, 0 }, total[3] = { 0, 0, 0 };
    __try {
        for (int i = 0; i < g_arena_count; i++) {
            unsigned char* c = (unsigned char*)g_arena_cats[i];
            // Side by team membership, not by faction: run 11 had a team-2 cat end
            // up with faction 0 and +0x365 cleared after an effect, and counting it
            // by faction made it a living side-1 cat, so the fight never ended.
            // Alive = not dead (+0x4C2) and still taking part (+0x365, the flag
            // the game's own victory/defeat checks require).
            unsigned char* cat = *(unsigned char**)(c + OFF_CATDATA);
            int side = cat ? team_of_cat(cat) : 0;
            if (!side) side = F(c, OFF_FACTION_BASE) == FACTION_ENEMIES ? 2 : 1;
            total[side]++;
            if (!c[OFF_CHAR_DEAD] && c[OFF_CHAR_ACTIVE]) alive[side]++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("referee: fault reading the arena cats, referee off");
        InterlockedExchange(&g_arena_active, 0);
        return;
    }
    if (total[1] == 0 || total[2] == 0) return;   // not a two-sided fight (yet)
    if (alive[1] && alive[2]) return;
    int winner = alive[1] ? 1 : 2;
    plog("referee: side %d wins (alive %d vs %d of %d vs %d), waiting for Enter / R",
         winner, alive[1], alive[2], total[1], total[2]);
    g_banner_ent = show_banner(winner == 1
        ? TRW(L"ИГРОК 1 ПОБЕДИЛ!\nESC — МЕНЮ: РЕВАНШ / ДОМОЙ", L"PLAYER 1 WINS!\nESC — MENU: REMATCH / HOME")
        : TRW(L"ИГРОК 2 ПОБЕДИЛ!\nESC — МЕНЮ: РЕВАНШ / ДОМОЙ", L"PLAYER 2 WINS!\nESC — MENU: REMATCH / HOME"));
    g_freeze_at = g_banner_ent ? GetTickCount64() + g_freeze_ms : 0;
    // Swallow a key already held at this moment (an Enter that ended the turn).
    g_key_enter_down = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    g_key_r_down = (GetAsyncKeyState('R') & 0x8000) != 0;
    g_end_at = 1;   // fight over: wait for a choice
}

// ---------------------------------------------------------------------------
// v5: mana for the enemy side. The portrait panel update FUN_14081AEE0(this)
// calls FUN_14097B000(panel, "faction is not a player faction") on the shown
// character, and the enemy layout it switches to has no mana bar. For the
// duration of that one UI call, flipped arena cats read as faction 1 again, so
// both teams get the full player panel. Same trick as the battle setup hook;
// nothing outside this call ever sees the 1.

#define RVA_PORTRAIT_UPDATE 0x81AEE0
static const unsigned char kPortraitEntry[14] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55 };
typedef void (__fastcall *fn_portrait_update)(void*);
static fn_portrait_update g_orig_portrait;

// mgmp build: StatusMenu::update is mgmp's detour (h_StatusMenu), which calls
// these two around the original instead of a second hook on the same function.
static void* g_lifted[MAX_ARENA_CATS];
static int   g_lifted_was[MAX_ARENA_CATS];   // the faction to put back
static int   g_lifted_n;
// Run 52 (online): StatusMenu::update is presentation, but a turn whose only
// difference between the peers was the number of frames drew different numbers of
// sim-stream values once a cat had been charmed (rng diverged, every cat row equal).
// During an arena the sim stream is put back after the panel's update, so the HUD
// can never move the simulation, however many frames each machine draws.
static unsigned char g_hud_rng[32];
static int           g_hud_rng_saved;

void pvp_status_menu_begin(void* panel) {
    void** lifted = g_lifted;
    int n = 0;
    (void)panel;
    g_lifted_n = 0;
    g_hud_rng_saved = 0;
    if (g_arena_active) {
        __try {
            unsigned char* r = sim_rng();
            if (r) { memcpy(g_hud_rng, r, 32); g_hud_rng_saved = 1; }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_arena_active) {
        // Online (owner): this player's own cats get the player panel with mana,
        // the opponent's cats the enemy panel without it -- on both machines,
        // whichever team each one is.
        int mine = g_arena_online ? (pvp_net_mode() == 1 ? 1 : pvp_net_mode() == 2 ? 2 : 0) : 0;
        __try {
            for (int i = 0; i < g_arena_count; i++) {
                unsigned char* c = (unsigned char*)g_arena_cats[i];
                int was = F(c, OFF_FACTION_BASE), want = was;
                if (mine) {
                    int t = team_of_cat(*(void**)(c + OFF_CATDATA));
                    if (t) want = t == mine ? FACTION_ALLIES : FACTION_ENEMIES;
                } else if (was == FACTION_ENEMIES && flipped_has(c)) {
                    want = FACTION_ALLIES;
                }
                if (want != was) {
                    F(c, OFF_FACTION_BASE) = want;
                    g_lifted_was[n] = was;
                    lifted[n++] = c;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_lifted_n = n;
}

void pvp_status_menu_end(void) {
    if (g_hud_rng_saved) {
        __try {
            unsigned char* r = sim_rng();
            if (r && memcmp(r, g_hud_rng, 32) != 0) {
                static int said;
                if (said++ < 5) plog("hud: StatusMenu::update moved the sim stream -- put back");
                memcpy(r, g_hud_rng, 32);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_hud_rng_saved = 0;
    }
    for (int i = 0; i < g_lifted_n; i++) F(g_lifted[i], OFF_FACTION_BASE) = g_lifted_was[i];
    g_lifted_n = 0;
}

// ---------------------------------------------------------------------------
// v10: real buttons, in the pause menu.
//
// PauseMenu::SetupMainSidebar (FUN_14028B620) registers its buttons through
// MenuPanel register_button (FUN_14097C070(panel, std::string anim, std::string
// label_key, std::function<void()> cb) -> Button*; all three by value). The
// "GiveUp" button (anim pb_abandon) is always registered; outside a run the game
// disables it (Button+0x2F0 = 4) and hides its clip (bit 0x20 of
// *(Button+0x48)+8), and it relabels it itself through
// FUN_140063720(Button+0x1B8, std::wstring*) for "abandon fight" / "skip
// tutorial", re-enabling it the same way (state 4 -> 5, bit 0x20 set).
// The callback is the std::function at Button+0xB8 (impl pointer at +0xF0).
//
// So: in the house the hidden GiveUp becomes "АРЕНА"; in an arena GiveUp reads
// "ДОМОЙ" (its click already goes home, see hook_transition) and QuitToMenu
// (anim pb_menu), when present, becomes "РЕВАНШ". Our buttons close the menu by
// running the Resume button's own callback, then act at the end of the frame.

#define RVA_REGISTER_BUTTON 0x97C070
#define RVA_SETUP_SIDEBAR   0x28B620
#define RVA_SET_LABEL       0x063720
// The std::function object starts at Button+0xB8 (0x38 bytes of storage) and its
// impl pointer is at +0xF0: Button::Click does `mov rcx,[button+0xF0]; call
// [rcx]->[+0x10]` (Mewgenics.exe+0x97E9B5). v10 first took +0xF0 for the object
// itself and crashed on the first click.
#define OFF_BTN_FUNC        0xB8
#define OFF_BTN_FUNC_IMPL   0xF0
#define OFF_BTN_STATE       0x2F0
#define OFF_BTN_CLIP        0x48
#define OFF_BTN_LABEL       0x1B8
static const unsigned char kRegisterButtonEntry[15] = {
    0x4C,0x89,0x4C,0x24,0x20, 0x4C,0x89,0x44,0x24,0x18, 0x48,0x89,0x54,0x24,0x10 };
static const unsigned char kSetupSidebarEntry[14] = {
    0x48,0x89,0x5C,0x24,0x10, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56 };

typedef void* (__fastcall *fn_register_button)(void*, msvc_string*, msvc_string*, void*);
typedef void  (__fastcall *fn_setup_sidebar)(void*, char);
typedef void  (__fastcall *fn_set_label)(void*, msvc_wstring*);
static fn_register_button g_orig_register_button;
static fn_setup_sidebar   g_orig_setup_sidebar;
static int g_in_sidebar;
static unsigned char *g_btn_resume, *g_btn_giveup, *g_btn_menu, *g_btn_quit;

enum { ACT_NONE, ACT_START_ARENA, ACT_REMATCH, ACT_HOME };
static volatile LONG g_pending_action;

// A std::function<void()> target, written in C: MSVC's _Func_base vtable is
// _Copy, _Move, _Do_call, _Target_type, _Delete_this, _Get. Our object lives in
// the std::function's small buffer, holds only an action code, and owns nothing.
typedef struct { void** vtbl; int action; } fn_impl;
static unsigned char g_dummy_type_info[32];
static void* __fastcall impl_copy(fn_impl* self, void* where) {
    fn_impl* d = (fn_impl*)where; d->vtbl = self->vtbl; d->action = self->action; return d;
}
static void  __fastcall impl_call(fn_impl* self);
static void* __fastcall impl_target_type(fn_impl* self) { (void)self; return g_dummy_type_info; }
static void  __fastcall impl_delete(fn_impl* self, char dealloc) { (void)self; (void)dealloc; }
static void* __fastcall impl_get(fn_impl* self) { return &self->action; }
static void* g_impl_vtbl[6] = { (void*)impl_copy, (void*)impl_copy, (void*)impl_call,
                                (void*)impl_target_type, (void*)impl_delete, (void*)impl_get };

static void run_button_callback(unsigned char* btn) {
    void** impl = *(void***)(btn + OFF_BTN_FUNC_IMPL);
    if (impl) ((void (__fastcall*)(void*))((*(void***)impl)[2]))(impl);
}

static void __fastcall impl_call(fn_impl* self) {
    plog("button: action %d clicked", self->action);
    if (g_btn_resume) run_button_callback(g_btn_resume);   // close the pause menu
    InterlockedExchange(&g_pending_action, self->action);
}

static void set_button(unsigned char* btn, const wchar_t* label, int action) {
    __try {
        msvc_wstring ws;
        memset(&ws, 0, sizeof ws);
        size_t n = wcslen(label);
        if (n > 7) n = 7;                                  // stay in the small buffer
        memcpy(ws.u.buf, label, n * sizeof(wchar_t));
        ws.size = n; ws.cap = 7;
        ((fn_set_label)((unsigned char*)GetModuleHandleA(NULL) + RVA_SET_LABEL))(btn + OFF_BTN_LABEL, &ws);
        if (*(int*)(btn + OFF_BTN_STATE) == 4) *(int*)(btn + OFF_BTN_STATE) = 5;
        unsigned char* clip = *(unsigned char**)(btn + OFF_BTN_CLIP);
        if (clip) clip[8] |= 0x20;
        if (action != ACT_NONE) {
            void** old = *(void***)(btn + OFF_BTN_FUNC_IMPL);
            if (old) ((void (__fastcall*)(void*, char))((*(void***)old)[4]))(old, (unsigned char*)old != btn + OFF_BTN_FUNC);
            fn_impl* mine = (fn_impl*)(btn + OFF_BTN_FUNC);
            mine->vtbl = g_impl_vtbl;
            mine->action = action;
            *(void**)(btn + OFF_BTN_FUNC_IMPL) = mine;
        }
        plog("button: %p relabelled (action %d)", btn, action);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("button: fault relabelling %p", btn);
    }
}

// Tint a button red-pink like the game's own GiveUp.
//
// Button::update (vtable slot 19, FUN_14097F6F0) caches the clip's transform once
// -- position/scale at Button+0x270 and the ColorTransform (8 floats: RGBA
// multipliers, RGBA offsets) at Button+0x288, flag Button+0x2A8 = cached -- and
// every frame tweens the clip's ColorTransform (clip+0x78) towards the hover colour
// or back to that cached base. v10.3 wrote only clip+0x78 and was tweened away;
// v10.4 tried state 5, which is just the "enable" animation and drops back to 0.
// So: write the cached base, mark it cached, and write the clip too.
// The values are the ones live memory showed on the game's own red-pink GiveUp.
#define OFF_CLIP_CXFORM   0x78
#define OFF_BTN_BASE_CX   0x288
#define OFF_BTN_BASE_SET  0x2A8
static const float kRedCx[8] = { 0.671f, 0.671f, 0.671f, 1.0f, 0.184f, 0.094f, 0.094f, 0.0f };
static void tint_button(unsigned char* btn) {
    __try {
        unsigned char* clip = *(unsigned char**)(btn + OFF_BTN_CLIP);
        if (!clip) return;
        memcpy(btn + OFF_BTN_BASE_CX, kRedCx, sizeof kRedCx);
        btn[OFF_BTN_BASE_SET] = 1;
        memcpy(clip + OFF_CLIP_CXFORM, kRedCx, sizeof kRedCx);
        plog("button: %p tinted (cached base + clip)", btn);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("button: fault tinting %p", btn);
    }
}

// Run 22: tinted for one frame, then white again. While the pause menu's open
// animation plays, Button::update re-caches the base transform from the clip every
// frame (its `|| parent animating` branch), and the timeline writes identity there.
// So the tint is re-applied after every Button::update of our two buttons, for as
// long as an arena runs. update is only ever called on live buttons, so this never
// touches a freed one. The clip itself is left to the game while the button is
// hovered or pressed (states 1, 2), so its hover highlight still shows.
#define RVA_BUTTON_UPDATE 0x97F6F0
static const unsigned char kButtonUpdateEntry[14] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x18, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55 };
typedef void (__fastcall *fn_button_update)(void*);
static fn_button_update g_orig_button_update;

void pvp_button_update(void* self) {
    unsigned char* btn = (unsigned char*)self;
    auto_button(btn);
    if (!g_arena_active || !btn || (btn != g_btn_menu && btn != g_btn_quit)) return;
    // Run 35: passive cards turned red. Once the pause menu closes, our buttons are
    // freed and the game puts other Buttons (passive cards) at the same address, so
    // the address alone is not enough: ours is the one whose callback is our impl.
    __try {
        fn_impl* impl = *(fn_impl**)(btn + OFF_BTN_FUNC_IMPL);
        if ((unsigned char*)impl != btn + OFF_BTN_FUNC || impl->vtbl != g_impl_vtbl) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    // Run 23: setting the cached flag ourselves (v10.5/v10.6) kept the game from
    // ever caching the button's position/scale at Button+0x270, which shares that
    // flag; hovering РЕВАНШ then tweened it towards a zero transform and it slid
    // over ДОМОЙ. So the flag is the game's: the red goes into the colour half of
    // the base only once the game has cached it.
    // Run 24 (live memory): the flag and both caches are only filled when a hover
    // starts (it is the hover tween's start point); before that the clip colour is
    // whatever the timeline left. So: flag 0 -> colour the clip only; flag 1 ->
    // colour the cached base too.
    __try {
        if (btn[OFF_BTN_BASE_SET]) memcpy(btn + OFF_BTN_BASE_CX, kRedCx, sizeof kRedCx);
        int st = *(int*)(btn + OFF_BTN_STATE);
        unsigned char* clip = *(unsigned char**)(btn + OFF_BTN_CLIP);
        if (clip && st != 1 && st != 2) memcpy(clip + OFF_CLIP_CXFORM, kRedCx, sizeof kRedCx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Hide a sidebar button the way the game hides GiveUp outside a run.
static void hide_button(unsigned char* btn) {
    __try {
        *(int*)(btn + OFF_BTN_STATE) = 4;
        unsigned char* clip = *(unsigned char**)(btn + OFF_BTN_CLIP);
        if (clip) clip[8] &= ~0x20;
        plog("button: %p hidden", btn);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("button: fault hiding %p", btn);
    }
}

static void* __fastcall hook_register_button(void* panel, msvc_string* anim, msvc_string* label, void* fn) {
    char name[32] = "";
    if (g_in_sidebar && anim) read_std_string((unsigned char*)anim, name, sizeof name);
    void* btn = g_orig_register_button(panel, anim, label, fn);
    if (g_in_sidebar) {
        if (!strcmp(name, "pb_resume")) g_btn_resume = (unsigned char*)btn;
        else if (!strcmp(name, "pb_abandon")) g_btn_giveup = (unsigned char*)btn;
        else if (!strcmp(name, "pb_menu")) g_btn_menu = (unsigned char*)btn;
        else if (!strcmp(name, "pb_quit")) g_btn_quit = (unsigned char*)btn;
    }
    return btn;
}

static void __fastcall hook_setup_sidebar(void* pause, char arg) {
    g_btn_resume = g_btn_giveup = g_btn_menu = g_btn_quit = NULL;
    g_in_sidebar = 1;
    g_orig_setup_sidebar(pause, arg);
    g_in_sidebar = 0;
    plog("pause menu: resume=%p giveup=%p menu=%p quit=%p arena=%d",
         g_btn_resume, g_btn_giveup, g_btn_menu, g_btn_quit, (int)g_arena_active);
    if (!g_btn_resume) return;
    if (g_arena_active && g_btn_menu && g_btn_quit) {
        // Run 18: the owner wants the two arena buttons next to each other. Slots 3
        // and 4 (QuitToMenu, Save & Quit) become РЕВАНШ and ДОМОЙ, slot 5 (GiveUp)
        // is hidden. This also takes away the two exits that would leave an arena
        // without restoring anything (and let the exit-time save write it).
        set_button(g_btn_menu, TRW(L"РЕВАНШ", L"REMATCH"), ACT_REMATCH);
        set_button(g_btn_quit, TRW(L"ДОМОЙ", L"HOME"), ACT_HOME);
        // Owner, run 19: both red -- applied by hook_button_update, not here.
        if (g_btn_giveup) hide_button(g_btn_giveup);
    } else if (g_arena_active) {
        if (g_btn_giveup) set_button(g_btn_giveup, TRW(L"ДОМОЙ", L"HOME"), ACT_NONE);
        if (g_btn_menu) set_button(g_btn_menu, TRW(L"РЕВАНШ", L"REMATCH"), ACT_REMATCH);
    } else if (find_house_scene() && g_btn_giveup) {
        set_button(g_btn_giveup, TRW(L"АРЕНА", L"ARENA"), ACT_START_ARENA);
    }
}

static void do_rematch(void);

static void pending_action_tick(void) {
    LONG a = InterlockedExchange(&g_pending_action, ACT_NONE);
    if (net_route_action(a)) return;
    if (a == ACT_START_ARENA && !g_arena_active) open_lobby();
    else if (a == ACT_REMATCH && g_arena_active) do_rematch();
    else if (a == ACT_HOME && g_arena_active) InterlockedExchange(&g_pending_exit, 1);
}

// Run 55 (online, diagnostic): the turn-15 hash differed on rng alone while every
// RECORDED draw of that turn matched -- from one shared state the host stepped the
// sim stream 6 times before the hash and the client 43, all through code the
// recorder cannot see (an inlined xoshiro step; ~100 functions have one). So during
// an online fight a hardware data breakpoint watches the first word of the sim
// stream: every step writes it, and each writer's address goes to pvp_rngwatch.log.
// The two peers' logs agree line for line until the culprit's first extra step.
#define RW_RING 16384
static unsigned long long g_rw_rip[RW_RING], g_rw_ret[RW_RING], g_rw_val[RW_RING];
#define RW_DEPTH 8
static unsigned long long g_rw_stk[RW_RING][RW_DEPTH];   // return addresses into the exe, nearest first
static volatile LONG g_rw_head, g_rw_tail;
static void*          g_rw_veh;
// Run 58: the RNG agrees now; the watch moved to the FACING of team 2's cats
// (Character+0x388, slots 0..3) -- a cat's facing changed on its owner only and
// a backstab differed 14 turns later. Log: pvp_facewatch.log.
static unsigned char* g_rw_addrs[4];
static int            g_rw_nslots;
static unsigned char  g_rw_slot[RW_RING];
static DWORD          g_rw_tid;
static int            g_rw_armed, g_rw_tried;
static FILE*          g_rw_log;
static HMODULE        g_rw_self;
static LONG           g_rw_lost;

static LONG CALLBACK rw_veh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    if (!(c->Dr6 & 0xF) || GetCurrentThreadId() != g_rw_tid) return EXCEPTION_CONTINUE_SEARCH;
    int slot = (c->Dr6 & 1) ? 0 : (c->Dr6 & 2) ? 1 : (c->Dr6 & 4) ? 2 : 3;
    LONG h = g_rw_head;
    if (h - g_rw_tail < RW_RING) {
        g_rw_rip[h % RW_RING] = c->Rip;
        g_rw_ret[h % RW_RING] = *(unsigned long long*)c->Rsp;
        g_rw_val[h % RW_RING] = *(unsigned long long*)g_rw_addrs[slot];
        g_rw_slot[h % RW_RING] = (unsigned char)slot;
        // Who asked, heuristically: stack words pointing just past a call in the
        // exe's code. Frames without one are skipped; enough to name the caller.
        unsigned long long exe = (unsigned long long)GetModuleHandleA(NULL);
        unsigned long long* sp = (unsigned long long*)c->Rsp;
        int k = 0;
        for (int w = 0; w < 400 && k < RW_DEPTH; w++) {
            unsigned long long v = sp[w];
            if (v < exe + 0x1000 || v >= exe + 0xD3E000) continue;
            const unsigned char* q = (const unsigned char*)v;
            if (q[-5] == 0xE8 || (q[-6] == 0xFF && q[-5] == 0x15) ||
                (q[-2] == 0xFF && (q[-1] & 0xF8) == 0xD0) ||
                (q[-3] == 0xFF && (q[-2] & 0x38) == 0x10 && (q[-2] & 0xC0) == 0x40) ||
                (q[-6] == 0xFF && (q[-5] & 0x38) == 0x10 && (q[-5] & 0xC0) == 0x80))
                g_rw_stk[h % RW_RING][k++] = v;
        }
        for (; k < RW_DEPTH; k++) g_rw_stk[h % RW_RING][k] = 0;
        g_rw_head = h + 1;
    } else {
        g_rw_lost++;
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// A thread cannot reliably set its own debug registers, so a helper suspends the
// game thread, writes slot 0 and resumes it.
static DWORD WINAPI rw_set_thread(LPVOID on) {
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_rw_tid);
    if (!t) return 1;
    DWORD ok = 0;
    if (SuspendThread(t) != (DWORD)-1) {
        CONTEXT c;
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(t, &c)) {
            c.Dr7 &= ~0xFFFF00FFull;                                  // slots 0..3: L/G, RW, LEN
            DWORD64* dr[4] = { &c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3 };
            for (int i = 0; i < 4; i++) {
                *dr[i] = 0;
                if (on && i < g_rw_nslots && g_rw_addrs[i]) {
                    *dr[i] = (DWORD64)g_rw_addrs[i];
                    c.Dr7 |= (1ull << (2 * i)) | (1ull << (16 + 4 * i)) | (3ull << (18 + 4 * i));   // write, 8 bytes
                }
            }
            ok = SetThreadContext(t, &c) ? 1 : 0;
        }
        ResumeThread(t);
    }
    CloseHandle(t);
    return ok ? 0 : 2;
}

static int rw_set(int on) {
    HANDLE th = CreateThread(NULL, 0, rw_set_thread, (LPVOID)(INT_PTR)on, 0, NULL);
    if (!th) return 0;
    WaitForSingleObject(th, 5000);
    DWORD code = 1;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    return code == 0;
}

static void rw_addr_str(unsigned long long a, char* out, size_t n) {
    unsigned long long exe = (unsigned long long)GetModuleHandleA(NULL);
    unsigned long long me = (unsigned long long)g_rw_self;
    if (a >= exe && a < exe + 0x1574000) snprintf(out, n, "e+%07llX", a - exe);
    else if (me && a >= me && a < me + 0x1000000) snprintf(out, n, "m+%06llX", a - me);
    else snprintf(out, n, "%llX", a);
}

static void rw_flush(void) {
    if (!g_rw_log) return;
    while (g_rw_tail != g_rw_head) {
        LONG i = g_rw_tail % RW_RING;
        char a[32], b[32];
        rw_addr_str(g_rw_rip[i], a, sizeof a);
        rw_addr_str(g_rw_ret[i], b, sizeof b);
        if (g_rw_slot[i] == 0)
            fprintf(g_rw_log, "%ld rng %s %s %016llx", g_rw_tail, a, b, g_rw_val[i]);
        else
            fprintf(g_rw_log, "%ld cat%d %s %s face(%d,%d)", g_rw_tail, g_rw_slot[i] - 1, a, b,
                    (int)(g_rw_val[i] & 0xFFFFFFFF), (int)(g_rw_val[i] >> 32));
        for (int k = 0; k < RW_DEPTH && g_rw_stk[i][k]; k++) {
            rw_addr_str(g_rw_stk[i][k], a, sizeof a);
            fprintf(g_rw_log, " %s", a);
        }
        fputc('\n', g_rw_log);
        g_rw_tail++;
    }
    if (g_rw_lost) { fprintf(g_rw_log, "!! %ld write(s) lost (ring full)\n", g_rw_lost); g_rw_lost = 0; }
    fflush(g_rw_log);
}

// Armed from the START until the fight is over; once per online fight.
// Off unless pvp.ini [debug] Watch=1: a release build sets no debug registers and
// installs no exception handler of its own.
static int g_rw_enabled;

static void rw_tick(void) {
    if (!g_rw_enabled) return;
    int want = g_arena_online && g_arena_active;
    int n2 = 0;
    unsigned char* t2[4];
    if (want && !g_rw_armed && !g_rw_tried) {
        __try {
            for (int i = 0; i < g_arena_count && n2 < 4; i++) {
                unsigned char* ch = (unsigned char*)g_arena_cats[i];
                if (team_of_cat(*(void**)(ch + OFF_CATDATA)) == 1) t2[n2++] = ch;   // run 59: the host's team diverged
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { n2 = 0; }
        if (n2 < g_team_size[0] || n2 == 0) want = 0;   // not all spawned yet
        if (n2 > 3) n2 = 3;                              // slot 0 is the sim stream
    }
    if (want && !g_rw_armed && !g_rw_tried) {
        g_rw_tried = 1;
        g_rw_nslots = n2 + 1;
        g_rw_addrs[0] = sim_rng();
        for (int i = 0; i < n2; i++) g_rw_addrs[i + 1] = t2[i] + OFF_CHAR_FACING;
        g_rw_tid = GetCurrentThreadId();
        if (!g_rw_veh) g_rw_veh = AddVectoredExceptionHandler(1, rw_veh);
        if (!g_rw_log) {
            char path[MAX_PATH];
            GetModuleFileNameA(g_rw_self, path, MAX_PATH);
            char* slash = strrchr(path, '\\');
            if (slash) strcpy(slash + 1, "pvp_facewatch.log");
            g_rw_log = fopen(path, "w");
        }
        g_rw_head = g_rw_tail = 0;
        g_rw_armed = rw_set(1);
        plog("net: facing watch %s (%d team-2 cat(s), thread %lu)", g_rw_armed ? "ARMED" : "could not arm",
             n2, g_rw_tid);
        if (g_rw_log) {
            fprintf(g_rw_log, "# fight start, armed=%d\n", g_rw_armed);
            for (int i = 0; i < n2; i++) {
                int x = 0, y = 0, tx = 0, ty = 0;
                __try {
                    x = *(int*)(t2[i] + OFF_CHAR_FACING); y = *(int*)(t2[i] + OFF_CHAR_FACING + 4);
                    unsigned char* to = *(unsigned char**)(t2[i] + OFF_CHAR_TOBJ);
                    tx = *(int*)(to + 0x48); ty = *(int*)(to + 0x4C);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
                fprintf(g_rw_log, "# cat%d at (%d,%d) facing (%d,%d)\n", i, tx, ty, x, y);
            }
            fflush(g_rw_log);
        }
    } else if (g_rw_armed && !(g_arena_online && g_arena_active)) {
        static int grace;
        if (g_arena_active || g_spawning) grace = 0;
        else if (++grace > 600) {   // not active for ~10 s: the fight is over
            rw_set(0);
            g_rw_armed = 0;
            rw_flush();
            if (g_rw_log) { fprintf(g_rw_log, "# fight over\n"); fflush(g_rw_log); }
            plog("net: facing watch off");
        }
    }
    if (!g_arena_online) g_rw_tried = 0;
    rw_flush();
}

void pvp_frame(void) {
    rw_tick();
    auto_tick();
    if (!find_house_scene() && !g_arena_active && !lobby_is_open()) return;   // nothing to do outside the house/arena
    int down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !g_f9_down && game_has_focus() && !g_arena_active && !lobby_is_open()) {
        if (pvp_net_mode()) open_lobby();   // online: never a one-machine quick fight
        else start_arena();
    }
    g_f9_down = down;
    pending_exit_tick();
    pending_action_tick();
    referee_tick();
    lobby_tick();
    bird_wave_tick();
}

static void __fastcall hook_house_init(void* house) {
    g_orig_house_init(house);
    plog("HousePlaceholder::init this=%p", house);
}

// Run 53 (online): a host cat moved, and on the HOST only it turned (0,-1) -> (1,0);
// the client's copy kept its facing. The next hit on it was a backstab on one
// machine and not on the other (27 vs 29 HP). The owner's PlayerBrain update
// (vtable slot 10, FUN_140778DA0: 8 touches of Character+0x388, two TacticsObject
// placements for the move/attack preview) turns the cat toward what the player is
// aiming at -- presentation, on the owner's machine only. mgmp freezes the same
// kind of write in Brain::UpdateDecision; this freezes it here: during an online
// arena the brain's character keeps the facing it had before the update.
#define RVA_PLAYERBRAIN_UPDATE 0x778DA0
#define OFF_BRAIN_CHARACTER    0x38
#define OFF_CHAR_FACING        0x388
static const unsigned char kPlayerBrainUpdateEntry[11] = {
    0x48,0x8B,0xC4, 0x55, 0x53, 0x56, 0x57, 0x41,0x54, 0x41,0x55 };
typedef void (__fastcall *fn_brain_update)(void*);
static fn_brain_update g_orig_pb_update;

static void __fastcall hook_pb_update(void* brain) {
    unsigned char* ch = NULL;
    unsigned long long facing = 0;
    if (g_arena_active && g_arena_online) {
        __try {
            ch = *(unsigned char**)((unsigned char*)brain + OFF_BRAIN_CHARACTER);
            if (ch) facing = *(unsigned long long*)(ch + OFF_CHAR_FACING);
        } __except (EXCEPTION_EXECUTE_HANDLER) { ch = NULL; }
    }
    if (ch) pvp_choice_fence(1);
    g_orig_pb_update(brain);
    if (ch) pvp_choice_fence(0);
    if (ch) {
        __try {
            if (*(unsigned long long*)(ch + OFF_CHAR_FACING) != facing) {
                static int said;
                if (said++ < 5) plog("net: PlayerBrain update turned a cat -- facing put back");
                *(unsigned long long*)(ch + OFF_CHAR_FACING) = facing;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// Run 54 (online): turn 0 already disagreed on two cats' tile elements (bit 0x10):
// the grass tiles themselves differed. A level's grass is rolled per tile when the
// level loads (tiles.gon "Grass": GrassTile 80 / TallGrassTile 15 / BlankTile 5), and
// that happens in FUN_14031E6E0 (the Battle scene's level load, called by the house-
// battle start FUN_140319170 right after FUN_14094F0A0 re-seeds the sim stream its own
// way) -- before Level::init, where the START's seed used to be set. So the seed now
// goes in at the level load's entry too, during an online start.
#define RVA_BATTLE_LEVEL_LOAD 0x31E6E0
static const unsigned char kLevelLoadEntry[9] = { 0x40,0x53, 0x55, 0x41,0x56, 0x48,0x83,0xEC,0x50 };
typedef void* (__fastcall *fn_level_load)(void*, void*, void*);
static fn_level_load g_orig_level_load;

static void* __fastcall hook_level_load(void* scene, void* a, void* path) {
    if (g_spawning && g_online_seed_set) {
        __try {
            unsigned char* r = sim_rng();
            if (r) { memcpy(r, g_online_seed, 32); plog("net: sim stream set from the START seed (level load)"); }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_orig_level_load(scene, a, path);
}

void pvp_init(HMODULE self) {
    g_rw_self = self;
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "pvp_arena.log");
    g_log = fopen(path, "w");
    InitializeCriticalSection(&g_lock);
    read_config(self);
    read_team_config(self);
    read_item_config(self);
    {
        char ini[MAX_PATH];
        pvp_ini_path(self, ini);
        g_cover_count = GetPrivateProfileIntA("arena", "Cover", 0, ini);
        g_cover_kind = GetPrivateProfileIntA("arena", "CoverKind", 0, ini);
        g_loot_count = GetPrivateProfileIntA("arena", "Loot", 0, ini);
        g_loot_mask = GetPrivateProfileIntA("arena", "LootMask", 7, ini);
        g_mob_count = GetPrivateProfileIntA("arena", "Mobs", 0, ini);
        g_mob_kind = GetPrivateProfileIntA("arena", "MobKind", 0, ini);
        g_mob_mode = GetPrivateProfileIntA("arena", "MobMode", 0, ini);
        g_mob_mad = GetPrivateProfileIntA("arena", "MobMadness", 1, ini);
        g_bird_count = GetPrivateProfileIntA("arena", "Birds", 0, ini);
        g_bird_mask = GetPrivateProfileIntA("arena", "BirdMask", 7, ini);
        g_bird_every = GetPrivateProfileIntA("arena", "BirdEvery", 0, ini);
        g_bird_wave = GetPrivateProfileIntA("arena", "BirdWave", 1, ini);
    }
    find_modpath();
    gd_start(plog);
    detect_language(self);
    lobby_set_english(g_en);
    auto_init(self);
    lobby_set_log(plog);
    plog("pvp arena (mgmp) v53 loaded, items %d/%d/%d, FreezeMs=%d, teams: '%s' vs '%s', TeamSize=%d",
         g_item_preset, g_items_per_cat, g_items_mirror, g_freeze_ms, g_team_room[0], g_team_room[1], g_team_max);

    if (!MJ_Require("pvp_probe") || !MJ_Resolve(&mj)) { plog("mewjector API missing"); return; }
    auto_hooks();
    intro_hooks(self);
    face_hooks();
    roll_x_hooks();
    // Setup first: a flipped cat must never reach battle setup unhooked.
    if (!hook(RVA_BATTLE_SETUP, kSetupEntry, 15, (void*)hook_battle_setup, (void**)&g_orig_setup, "battle setup"))
        return;
    hook(RVA_CHARACTER_INIT, kInitEntry, 15, (void*)hook_char_init, (void**)&g_orig_init, "Character init");
    hook(RVA_FIND_ITEM_FROM_POOL, kFindItemEntry, 15, (void*)hook_find_item, (void**)&g_orig_find_item,
         "FindItemFromPool");
    hook(RVA_ROUND_UPKEEP, kRoundUpkeepEntry, 15, (void*)hook_round_upkeep, (void**)&g_orig_round_upkeep,
         "Character::RoundUpkeep");

    g_start_house_fight = (fn_start_house_fight)((unsigned char*)GetModuleHandleA(NULL) + RVA_START_HOUSE_FIGHT);
    hook(RVA_HOUSE_INIT, kHouseInitEntry, 14, (void*)hook_house_init, (void**)&g_orig_house_init,
         "HousePlaceholder::init");
    hook(RVA_BUILD_HOUSE, kBuildHouseEntry, 16, (void*)hook_build_house, (void**)&g_orig_build_house,
         "House builder");
    // Button::update: not hooked here -- mgmp's h_ButtonUpdate calls pvp_button_update.
    hook(RVA_REGISTER_BUTTON, kRegisterButtonEntry, 15, (void*)hook_register_button,
         (void**)&g_orig_register_button, "MenuPanel register_button");
    hook(RVA_SETUP_SIDEBAR, kSetupSidebarEntry, 14, (void*)hook_setup_sidebar,
         (void**)&g_orig_setup_sidebar, "PauseMenu::SetupMainSidebar");
    hook(RVA_LEVEL_INIT, kLevelInitEntry, 15, (void*)hook_level_init, (void**)&g_orig_level_init,
         "Level::init");
    hook(RVA_PLACE_TILE, kPlaceTileEntry, 15, (void*)hook_place_tile, (void**)&g_orig_place_tile,
         "TacticsObject place");
    hook(RVA_TRANSITION, kTransitionEntry, 16, (void*)hook_transition, (void**)&g_orig_transition,
         "MewDirector::Transition");
    hook(RVA_GEN_CATS, kGenCatsEntry, 15, (void*)hook_gen_cats, (void**)&g_orig_gen_cats,
         "DebugGenAdventureCats");
    pvp_clone_init();
    hook(RVA_BATTLE_LEVEL_LOAD, kLevelLoadEntry, 9, (void*)hook_level_load, (void**)&g_orig_level_load,
         "Battle level load (online seed)");
    hook(RVA_PLAYERBRAIN_UPDATE, kPlayerBrainUpdateEntry, 11, (void*)hook_pb_update, (void**)&g_orig_pb_update,
         "PlayerBrain update (facing freeze, online)");
    // StatusMenu::update (the portrait panel): mgmp's h_StatusMenu calls
    // pvp_status_menu_begin / _end around the original.
}

// ---------------------------------------------------------------------------
// Online (mgmp net.mode = "pvp"). Two players, each in their OWN house.
//
// Lobby: whoever presses АРЕНА opens it on both machines (LOBBY_OPEN). Each player
// picks cats from their own house -- the host for team 1, the client for team 2 --
// and both keep sending what they picked (PICK: lobby rows for display plus each
// cat's SerializeCatData bytes). The host's rules win (SETTINGS). Either can cancel
// (LOBBY_CLOSE).
//
// Start: only the host. It builds the fight's whole input once -- the level file
// (cover / loot / monsters / birds already placed), a 32-byte sim-stream seed, a
// battle id, every fighter's bytes in order, every fighter's item kit -- sends it
// (START) and then applies exactly the same bytes itself through arena_online_apply,
// which is all the client does too. So both machines build byte-identical clones,
// load the same level, start Level::init from the same sim stream and give mgmp's
// lockstep the same battle id; the control split goes by team (host = team 1).
//
// РЕВАНШ: the host rebuilds the input (new level, seed, id, kits; same fighters) and
// sends a START with the rematch flag; a client's РЕВАНШ is a request to the host.
// ДОМОЙ: either side, both go home (ACTION).

enum { SUB_LOBBY_OPEN = 1, SUB_LOBBY_CLOSE = 2, SUB_SETTINGS = 3, SUB_PICK = 4, SUB_START = 5, SUB_ACTION = 6 };
enum { NETACT_REMATCH = 1, NETACT_HOME = 2 };
#define NET_MAX_SIDE 4
#define NET_MAX_FIGHTERS (2 * NET_MAX_SIDE)

typedef struct { unsigned char* p; uint32_t n, cap; int ok; } wbuf;
static void wb_put(wbuf* b, const void* d, uint32_t n) {
    if (!b->ok) return;
    if (b->n + n > b->cap) {
        uint32_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->n + n) c *= 2;
        unsigned char* q = (unsigned char*)realloc(b->p, c);
        if (!q) { b->ok = 0; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void wb_u8(wbuf* b, unsigned v)   { unsigned char c = (unsigned char)v; wb_put(b, &c, 1); }
static void wb_u32(wbuf* b, uint32_t v)  { wb_put(b, &v, 4); }
static void wb_u64(wbuf* b, uint64_t v)  { wb_put(b, &v, 8); }

typedef struct { const unsigned char* p; uint32_t n, pos; int ok; } rbuf;
static const unsigned char* rb_ptr(rbuf* r, uint32_t n) {
    if (!r->ok || r->pos + n > r->n) { r->ok = 0; return NULL; }
    const unsigned char* q = r->p + r->pos;
    r->pos += n;
    return q;
}
static void rb_get(rbuf* r, void* d, uint32_t n) {
    const unsigned char* q = rb_ptr(r, n);
    if (q) memcpy(d, q, n); else memset(d, 0, n);
}
static uint32_t rb_u8(rbuf* r)  { unsigned char c; rb_get(r, &c, 1); return c; }
static uint32_t rb_u32(rbuf* r) { uint32_t v; rb_get(r, &v, 4); return v; }
static uint64_t rb_u64(rbuf* r) { uint64_t v; rb_get(r, &v, 8); return v; }

static uint64_t fnv64(const void* p, uint32_t n, uint64_t h) {
    const unsigned char* b = (const unsigned char*)p;
    for (uint32_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ULL; }
    return h;
}
static uint64_t rnd64(void) { return ((uint64_t)rnd(0x7FFFFFFF) << 33) ^ ((uint64_t)rnd(0x7FFFFFFF) << 2) ^ rnd(4); }

// The fight's per-fight deterministic RNG (bird rewards): seeded from START.
static uint64_t g_fight_rng;
static unsigned int rnd_fight(unsigned int n) {
    if (!g_online_seed_set) return rnd(n);
    g_fight_rng ^= g_fight_rng << 13; g_fight_rng ^= g_fight_rng >> 7; g_fight_rng ^= g_fight_rng << 17;
    return n ? (unsigned int)(g_fight_rng % n) : 0;
}

// What the other player picked (their PICK), and what the last START fought with.
static lobby_cat      g_remote[NET_MAX_SIDE];
static unsigned char* g_remote_bytes[NET_MAX_SIDE];
static uint32_t       g_remote_len[NET_MAX_SIDE];
static int            g_remote_n;
static char           g_remote_name[32];

static lobby_cat      g_fight_rows[NET_MAX_FIGHTERS];
static unsigned char* g_fight_bytes[NET_MAX_FIGHTERS];
static uint32_t       g_fight_len[NET_MAX_FIGHTERS];
static int            g_fight_n1, g_fight_n2;
static lobby_settings g_fight_settings;

static uint64_t           g_pick_sent_hash, g_set_sent_hash;
static unsigned long long g_sync_at;
static int                g_lobby_announce = 1;

static void net_reset_sync(void) { g_pick_sent_hash = g_set_sent_hash = 0; g_sync_at = 0; }

static void net_send_pick_and_rules(void) {
    lobby_cat mine[LOBBY_MAX_CATS];
    char name[32];
    lobby_settings s;
    int n = lobby_get_local(mine, name, &s);
    if (n > NET_MAX_SIDE) n = NET_MAX_SIDE;
    uint64_t h = fnv64(name, 32, 0xCBF29CE484222325ULL);
    h = fnv64(&n, sizeof n, h);
    for (int i = 0; i < n; i++) {
        h = fnv64(&mine[i].key, sizeof mine[i].key, h);
        h = fnv64(mine[i].kit, sizeof mine[i].kit, h);
    }
    uint64_t hs = fnv64(&s, sizeof s, 0xCBF29CE484222325ULL);
    unsigned long long now = GetTickCount64();
    int periodic = now >= g_sync_at;   // a peer that opened its lobby late still hears us
    if (h != g_pick_sent_hash || periodic) {
        wbuf b = { NULL, 0, 0, 1 };
        wb_put(&b, name, 32);
        wb_u8(&b, (unsigned)n);
        for (int i = 0; i < n; i++) {
            uint8_t* bytes = NULL;
            uint32_t len = pvp_clone_serialize(mine[i].cat, &bytes);
            wb_put(&b, &mine[i], sizeof(lobby_cat));
            wb_u32(&b, len);
            if (len) wb_put(&b, bytes, len);
            free(bytes);
        }
        if (b.ok && pvp_net_send(SUB_PICK, b.p, b.n)) g_pick_sent_hash = h;
        free(b.p);
    }
    if (pvp_net_mode() == 1 && (hs != g_set_sent_hash || periodic)) {
        if (pvp_net_send(SUB_SETTINGS, &s, sizeof s)) g_set_sent_hash = hs;
    }
    if (periodic) g_sync_at = now + 2000;
}

static void net_on_pick(const unsigned char* data, uint32_t size) {
    rbuf r = { data, size, 0, 1 };
    char name[32];
    rb_get(&r, name, 32);
    name[31] = 0;
    int n = (int)rb_u8(&r);
    if (n > NET_MAX_SIDE) { plog("net: !! PICK with %d cats -- ignored", n); return; }
    lobby_cat rows[NET_MAX_SIDE];
    const unsigned char* bytes[NET_MAX_SIDE];
    uint32_t len[NET_MAX_SIDE];
    for (int i = 0; i < n; i++) {
        rb_get(&r, &rows[i], sizeof(lobby_cat));
        len[i] = rb_u32(&r);
        bytes[i] = len[i] ? rb_ptr(&r, len[i]) : NULL;
    }
    if (!r.ok) { plog("net: !! PICK did not parse"); return; }
    for (int i = 0; i < g_remote_n; i++) { free(g_remote_bytes[i]); g_remote_bytes[i] = NULL; }
    for (int i = 0; i < n; i++) {
        g_remote[i] = rows[i];
        g_remote[i].cat = NULL;   // a pointer into the other process
        g_remote_len[i] = 0;
        g_remote_bytes[i] = len[i] ? (unsigned char*)malloc(len[i]) : NULL;
        if (g_remote_bytes[i]) { memcpy(g_remote_bytes[i], bytes[i], len[i]); g_remote_len[i] = len[i]; }
    }
    g_remote_n = n;
    memcpy(g_remote_name, name, 32);
    lobby_set_remote(name, g_remote, n);
}

// Item kits for the fighters, rolled by the host (the client never rolls).
static void roll_kits(const lobby_settings* s, int n1, int n2, char kits[][NUM_SLOTS][48]) {
    memset(kits, 0, sizeof(char) * NET_MAX_FIGHTERS * NUM_SLOTS * 48);
    if (s->item_preset == PRESET_NONE) return;
    if (s->item_preset == PRESET_HOUSE) {
        // Online "Из дома": each player dressed their own cats from their own
        // storage in the lobby; the kits came with the rows (the host's own, and
        // the client's in its PICK). Built by name on both machines, so neither
        // house loses anything.
        for (int i = 0; i < n1 + n2; i++)
            for (int sl = 0; sl < NUM_SLOTS; sl++) {
                strncpy(kits[i][sl], g_fight_rows[i].kit[sl], 47);
                kits[i][sl][47] = 0;
            }
        return;
    }
    int mirror = s->item_preset == PRESET_BALANCED;
    int per = s->items_per_cat < 1 ? 1 : s->items_per_cat > NUM_SLOTS ? NUM_SLOTS : s->items_per_cat;
    for (int i = 0; i < n1 + n2; i++) {
        if (mirror && i >= n1 && i - n1 < n1) { memcpy(kits[i], kits[i - n1], sizeof kits[i]); continue; }
        int order[NUM_SLOTS] = { 0, 1, 2, 3, 4 };
        for (int k = NUM_SLOTS - 1; k > 0; k--) { int j = rnd(k + 1); int t = order[k]; order[k] = order[j]; order[j] = t; }
        for (int k = 0; k < per; k++) {
            int sl = order[k];
            int pool = gd_slot_count(sl);
            if (!pool) continue;
            strncpy(kits[i][sl], gd_slot_item(sl, (int)rnd(pool)), 47);
        }
    }
}

static int split_owner(const void* character) {
    __try {
        void* cat = *(void**)((const unsigned char*)character + OFF_CATDATA);
        int t = cat ? team_of_cat(cat) : 0;
        return t == 1 ? 0 : t == 2 ? 1 : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static void arena_online_apply(const unsigned char* data, uint32_t size);

// Host: build the fight's input, send it, apply it here too.
static void host_start(int rematch) {
    lobby_settings* s = &g_fight_settings;
    // The level: placed with the host's rules, then shipped as bytes.
    g_level_fixed[0] = 0;
    g_cover_count = s->cover_count; g_cover_kind = s->cover_kind;
    g_loot_count = s->loot_count;   g_loot_mask = s->loot_mask;
    g_mob_count = s->mob_count;     g_mob_kind = s->mob_kind;
    g_bird_count = s->bird_count;   g_bird_mask = s->bird_mask;
    const char* lvl = arena_level_for_fight();
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\levels\\%s", g_modpath, lvl);
    for (char* c = path; *c; c++) if (*c == '/') *c = '\\';
    unsigned char* lvl_bytes = NULL;
    uint32_t lvl_len = 0;
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && n < 1 << 20 && (lvl_bytes = (unsigned char*)malloc(n)) && fread(lvl_bytes, 1, n, f) == (size_t)n) lvl_len = (uint32_t)n;
        fclose(f);
    }
    if (!lvl_len) { plog("net: !! cannot read the level %s -- not starting", path); free(lvl_bytes); return; }

    char kits[NET_MAX_FIGHTERS][NUM_SLOTS][48];
    roll_kits(s, g_fight_n1, g_fight_n2, kits);
    unsigned char seed[32];
    for (int i = 0; i < 4; i++) { uint64_t v = rnd64(); memcpy(seed + 8 * i, &v, 8); }
    uint64_t battle_id = rnd64() | 1;

    wbuf b = { NULL, 0, 0, 1 };
    wb_u8(&b, rematch ? 1 : 0);
    wb_put(&b, s, sizeof *s);
    wb_u64(&b, battle_id);
    wb_put(&b, seed, 32);
    wb_u32(&b, lvl_len);
    wb_put(&b, lvl_bytes, lvl_len);
    wb_u8(&b, (unsigned)g_fight_n1);
    wb_u8(&b, (unsigned)g_fight_n2);
    for (int i = 0; i < g_fight_n1 + g_fight_n2; i++) {
        wb_put(&b, &g_fight_rows[i], sizeof(lobby_cat));
        wb_u32(&b, g_fight_len[i]);
        wb_put(&b, g_fight_bytes[i], g_fight_len[i]);
        wb_put(&b, kits[i], sizeof kits[i]);
    }
    free(lvl_bytes);
    if (!b.ok) { plog("net: !! START did not build"); free(b.p); return; }
    int sent = pvp_net_send(SUB_START, b.p, b.n);
    plog("net: START %s (%u bytes, battle %016llx, %d vs %d) %s", rematch ? "rematch" : "fight", b.n,
         (unsigned long long)battle_id, g_fight_n1, g_fight_n2, sent ? "sent" : "NOT SENT");
    if (sent) arena_online_apply(b.p, b.n);
    free(b.p);
}

// Host pressed СТАРТ in the online lobby: fix the fighters (own picks + the remote's).
static void host_start_from_lobby(const lobby_cat* own, int n_own, const lobby_settings* s) {
    for (int i = 0; i < g_fight_n1 + g_fight_n2; i++) { free(g_fight_bytes[i]); g_fight_bytes[i] = NULL; }
    g_fight_n1 = g_fight_n2 = 0;
    g_fight_settings = *s;
    int k = 0;
    for (int i = 0; i < n_own && g_fight_n1 < NET_MAX_SIDE; i++) {
        if (own[i].team != 1) continue;
        uint8_t* bytes = NULL;
        uint32_t len = pvp_clone_serialize(own[i].cat, &bytes);
        if (!len) { plog("net: !! could not serialize %s", own[i].name); continue; }
        g_fight_rows[k] = own[i]; g_fight_rows[k].cat = NULL;
        g_fight_bytes[k] = bytes; g_fight_len[k] = len;
        k++; g_fight_n1++;
    }
    for (int i = 0; i < g_remote_n && g_fight_n2 < NET_MAX_SIDE; i++) {
        if (!g_remote_len[i]) continue;
        g_fight_rows[k] = g_remote[i];
        g_fight_bytes[k] = (unsigned char*)malloc(g_remote_len[i]);
        if (!g_fight_bytes[k]) continue;
        memcpy(g_fight_bytes[k], g_remote_bytes[i], g_remote_len[i]);
        g_fight_len[k] = g_remote_len[i];
        k++; g_fight_n2++;
    }
    if (!g_fight_n1 || !g_fight_n2) { plog("net: !! start with %d vs %d cats -- not starting", g_fight_n1, g_fight_n2); return; }
    host_start(0);
}

// Both machines: build the fight from a START, identically.
static void arena_online_apply(const unsigned char* data, uint32_t size) {
    rbuf r = { data, size, 0, 1 };
    int rematch = (int)rb_u8(&r);
    lobby_settings s;
    rb_get(&r, &s, sizeof s);
    uint64_t battle_id = rb_u64(&r);
    unsigned char seed[32];
    rb_get(&r, seed, 32);
    uint32_t lvl_len = rb_u32(&r);
    const unsigned char* lvl = rb_ptr(&r, lvl_len);
    int n1 = (int)rb_u8(&r), n2 = (int)rb_u8(&r);
    if (!r.ok || n1 < 1 || n2 < 1 || n1 > NET_MAX_SIDE || n2 > NET_MAX_SIDE) { plog("net: !! START did not parse"); return; }
    lobby_cat rows[NET_MAX_FIGHTERS];
    const unsigned char* bytes[NET_MAX_FIGHTERS];
    uint32_t blen[NET_MAX_FIGHTERS];
    char kits[NET_MAX_FIGHTERS][NUM_SLOTS][48];
    for (int i = 0; i < n1 + n2; i++) {
        rb_get(&r, &rows[i], sizeof(lobby_cat));
        blen[i] = rb_u32(&r);
        bytes[i] = rb_ptr(&r, blen[i]);
        rb_get(&r, kits[i], sizeof kits[i]);
    }
    if (!r.ok) { plog("net: !! START fighters did not parse"); return; }
    if (rematch && !g_arena_active) { plog("net: rematch START outside an arena -- ignored"); return; }
    if (!rematch && g_arena_active) { plog("net: START while an arena is on -- ignored"); return; }
    unsigned char* house = rematch ? NULL : find_house_scene();
    unsigned char* md = *(unsigned char**)((unsigned char*)GetModuleHandleA(NULL) + RVA_MEWDIRECTOR_PTR);
    unsigned char* battle = rematch ? find_scene("Battle") : NULL;
    if (!md || (!rematch && !house) || (rematch && !battle)) { plog("net: !! START but no house / battle / MewDirector"); return; }

    // The level, byte for byte.
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\levels\\pvp\\g.lvl", g_modpath);
    FILE* f = fopen(path, "wb");
    if (!f) { plog("net: !! cannot write %s", path); return; }
    fwrite(lvl, 1, lvl_len, f);
    fclose(f);
    strcpy(g_level_fixed, "pvp/g.lvl");

    // The rules that act during the fight.
    memcpy(g_player_name, s.player_name, sizeof g_player_name);
    g_money_on = s.money_on; g_money = s.money;
    g_mob_count = s.mob_count; g_mob_kind = s.mob_kind; g_mob_mode = s.mob_mode; g_mob_mad = s.mob_mad;
    g_bird_count = s.bird_count; g_bird_mask = s.bird_mask; g_bird_every = s.bird_every; g_bird_wave = s.bird_wave;
    g_item_preset = PRESET_HOUSE;   // every kit is fixed by the host: the "picked" path builds them
    g_house_picked = 1;

    // The fighters: clones from the shipped bytes, team 1 first.
    restore_items();                // the previous fight's arena items off the clone objects first
    pvp_clone_clear();
    g_cat_snap_count = 0;
    g_team_size[0] = g_team_size[1] = 0;
    for (int i = 0; i < n1 + n2; i++) {
        int t = i < n1 ? 0 : 1;
        void* clone = pvp_clone_from_bytes(i, bytes[i], blen[i]);
        if (!clone) { plog("net: !! clone %d (%s) failed -- not starting", i, rows[i].name); return; }
        // Run 51: the two houses are on different days, and a cat's age (life stage,
        // so its stats) is the house's current day minus the cat's birthday. The
        // owner's age rides in the row; the birthday is re-based on this machine's day.
        __try {
            long long today = *(long long*)(md + 0x580);
            *(long long*)((unsigned char*)clone + 0xC38) = today - rows[i].age_days;
        } __except (EXCEPTION_EXECUTE_HANDLER) { plog("net: !! could not re-base the age of clone %d", i); }
        memcpy(g_pick_kit[g_cat_snap_count], kits[i], sizeof kits[i]);
        snapshot_cat((unsigned char*)clone, t, g_team_size[t]);
        g_team_size[t]++;
        plog("net: fighter %d team %d %s (%u bytes)", i, t + 1, rows[i].name, blen[i]);
    }

    memcpy(g_online_seed, seed, 32);
    g_online_seed_set = 1;
    g_fight_rng = fnv64(seed, 32, 0xCBF29CE484222325ULL) | 1;
    g_arena_online = 1;
    pvp_lockstep_split(split_owner);
    pvp_lockstep_enter(battle_id);
    plog("net: %s battle %016llx, %d vs %d, seed %02x%02x%02x%02x...", rematch ? "rematch" : "start",
         (unsigned long long)battle_id, n1, n2, seed[0], seed[1], seed[2], seed[3]);

    if (rematch) {
        g_end_at = 0;
        g_banner_ent = NULL;
        g_freeze_at = 0;
        apply_items();
        start_fight(battle, 0);
    } else {
        if (lobby_is_open()) lobby_close();
        start_arena_with_teams(house, md);
    }
}

// --- the hooks into the hotseat code -----------------------------------------

static int net_open_lobby(const lobby_cat* cats, int n, const lobby_settings* s) {
    int mode = pvp_net_mode();
    if (!mode) return 0;
    lobby_open_online(cats, n, s, mode == 1);
    if (g_remote_n) lobby_set_remote(g_remote_name, g_remote, g_remote_n);
    net_reset_sync();
    if (g_lobby_announce) pvp_net_send(SUB_LOBBY_OPEN, NULL, 0);
    plog("net: lobby opened online as %s%s", mode == 1 ? "host" : "client", g_lobby_announce ? "" : " (asked by the other player)");
    return 1;
}

static int net_lobby_tick(void) {
    if (lobby_take_cancel()) {
        pvp_net_send(SUB_LOBBY_CLOSE, NULL, 0);
        plog("net: lobby cancelled here -- told the other player");
        return 1;
    }
    if (!lobby_is_online()) return 0;
    if (!pvp_net_mode()) { plog("net: session gone -- closing the online lobby"); lobby_close(); return 1; }
    if (lobby_is_open()) net_send_pick_and_rules();
    static lobby_cat chosen[LOBBY_MAX_CATS];
    int n = 0;
    lobby_settings s;
    if (pvp_net_mode() == 1 && lobby_take_start(chosen, &n, &s)) host_start_from_lobby(chosen, n, &s);
    return 1;
}

static int net_route_action(long a) {
    int mode = g_arena_online ? pvp_net_mode() : 0;
    if (!mode || !g_arena_active) return 0;
    if (a == ACT_REMATCH) {
        if (mode == 1) host_start(1);
        else { unsigned char v = NETACT_REMATCH; pvp_net_send(SUB_ACTION, &v, 1); plog("net: rematch asked of the host"); }
        return 1;
    }
    if (a == ACT_HOME) {
        unsigned char v = NETACT_HOME;
        pvp_net_send(SUB_ACTION, &v, 1);
        InterlockedExchange(&g_pending_exit, 1);
        return 1;
    }
    return 0;
}

static int net_end_choice(int rematch) {
    if (!g_arena_online || !pvp_net_mode()) return 0;
    InterlockedExchange(&g_pending_action, rematch ? ACT_REMATCH : ACT_HOME);
    return 1;
}

void pvp_on_message(uint8_t from, uint8_t sub, const uint8_t* data, uint32_t size) {
    __try {
        switch (sub) {
            case SUB_LOBBY_OPEN:
                if (!lobby_is_open() && !g_arena_active && find_house_scene()) {
                    g_lobby_announce = 0;
                    open_lobby();
                    g_lobby_announce = 1;
                }
                break;
            case SUB_LOBBY_CLOSE:
                if (lobby_is_open() && lobby_is_online()) { lobby_close(); plog("net: the other player closed the lobby"); }
                break;
            case SUB_SETTINGS:
                if (size == sizeof(lobby_settings) && pvp_net_mode() == 2 && lobby_is_online())
                    lobby_set_settings((const lobby_settings*)data);
                break;
            case SUB_PICK:
                net_on_pick(data, size);
                break;
            case SUB_START:
                if (pvp_net_mode() == 2) arena_online_apply(data, size);
                break;
            case SUB_ACTION:
                if (size >= 1 && data[0] == NETACT_HOME && g_arena_active) {
                    plog("net: the other player went home");
                    InterlockedExchange(&g_pending_exit, 1);
                } else if (size >= 1 && data[0] == NETACT_REMATCH && pvp_net_mode() == 1 && g_arena_active) {
                    plog("net: the client asks for a rematch");
                    host_start(1);
                }
                break;
            default:
                plog("net: unknown arena message %u from peer %u (%u bytes)", (unsigned)sub, (unsigned)from, size);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        plog("net: !! fault handling arena message %u", (unsigned)sub);
    }
}

// ---------------------------------------------------------------------------
// Auto mode (pvp.ini [auto] Enabled=1): unattended online fights for soak
// testing. Both peers run it. The host opens the lobby, rolls random rules and
// starts once both sides have auto-picked their cats; the client only picks. In
// the fight the game's AI decides for each peer's own cats (mgmp's GetChoice hook,
// pvp_auto_on). After a fight the host starts a rematch with freshly rolled
// rules, and every HomeEvery fights goes home for new cats. Each event is a line
// in pvp_auto.log; a halted lockstep stops the loop (the soak script collects the
// logs and restarts both copies).
static int                g_auto, g_auto_home_every = 3, g_auto_fight_minutes = 12;
static FILE*              g_auto_log;
static unsigned long long g_auto_t, g_auto_fight_t0, g_auto_end_seen;
static int                g_auto_fights, g_auto_since_home, g_auto_halted, g_auto_rolled, g_auto_in_fight;
static int                g_auto_dress_tried;   // once per lobby: an empty storage stays empty

static void alog(const char* fmt, ...) {
    if (!g_auto_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_auto_log, "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_auto_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_auto_log);
    fflush(g_auto_log);
}

static void auto_init(HMODULE self) {
    char ini[MAX_PATH];
    pvp_ini_path(self, ini);
    g_auto = GetPrivateProfileIntA("auto", "Enabled", 0, ini);
    g_rw_enabled = GetPrivateProfileIntA("debug", "Watch", 0, ini);
    g_auto_home_every = GetPrivateProfileIntA("auto", "HomeEvery", 3, ini);
    g_auto_fight_minutes = GetPrivateProfileIntA("auto", "FightMinutes", 12, ini);
    if (!g_auto) return;
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "pvp_auto.log");
    g_auto_log = fopen(path, "a");
    alog("=== auto mode on (home every %d fight(s), fight limit %d min)", g_auto_home_every, g_auto_fight_minutes);
}

static const char* const kPresetNames[] = { "none", "random", "house", "balanced" };

static void auto_roll(lobby_settings* s, int keep_team) {
    static const int kPresets[4] = { PRESET_NONE, PRESET_RANDOM, PRESET_HOUSE, PRESET_BALANCED };
    if (!keep_team) s->team_max = 1 + (int)rnd(4);
    // A rematch keeps the cats, and the house kits were dressed in the lobby: it
    // keeps "Из дома" only if the lobby had it.
    if (!keep_team) s->item_preset = kPresets[rnd(4)];
    else if (s->item_preset != PRESET_HOUSE) s->item_preset = kPresets[(int[]){0, 1, 3}[rnd(3)]];
    s->items_per_cat = 1 + (int)rnd(5);
    s->money_on = (int)rnd(2);
    s->money = 10 * (int)rnd(6);
    s->cover_count = 2 * (int)rnd(7);
    s->cover_kind = (int)rnd(3);
    s->loot_count = (int)rnd(11);
    s->loot_mask = 1 + (int)rnd(15);
    s->mob_count = (int)rnd(7);
    s->mob_kind = (int)rnd(3);
    s->mob_mode = (int)rnd(2);
    s->mob_mad = (int)rnd(2);
    s->bird_count = (int)rnd(5);
    s->bird_mask = 1 + (int)rnd(7);
    s->bird_every = (int)rnd(4);        // 0 = only the starting birds
    s->bird_wave = 1 + (int)rnd(3);
}

static void auto_describe(const lobby_settings* s, char* out, size_t n) {
    _snprintf(out, n, "%dv%d items %s x%d money %s%d cover %d/k%d loot %d/m%d mobs %d/k%d/mode%d%s birds %d/m%d waves every %d x%d",
              s->team_max, s->team_max, kPresetNames[s->item_preset & 3], s->items_per_cat,
              s->money_on ? "" : "off/", s->money, s->cover_count, s->cover_kind, s->loot_count, s->loot_mask,
              s->mob_count, s->mob_kind, s->mob_mode, s->mob_mad ? "/mad" : "", s->bird_count, s->bird_mask,
              s->bird_every, s->bird_wave);
}

int pvp_auto_on(void) {
    return g_auto && g_arena_online && g_arena_active && !g_auto_halted;
}

static void auto_tick(void) {
    if (!g_auto) return;
    int mode = pvp_net_mode();
    if (!mode) return;
    unsigned long long now = GetTickCount64();
    if (pvp_lockstep_halted()) {
        if (!g_auto_halted) { alog("HALT in fight %d -- auto mode stopped", g_auto_fights); g_auto_halted = 1; }
        return;
    }
    if (g_auto_halted || now < g_auto_t) return;

    // The lobby: rules (host), picks (both), start (host).
    if (lobby_is_open() && lobby_is_online()) {
        lobby_cat mine[LOBBY_MAX_CATS];
        char name[32];
        lobby_settings s;
        int n = lobby_get_local(mine, name, &s);
        if (mode == 1 && !g_auto_rolled) {
            auto_roll(&s, 0);
            lobby_set_settings(&s);
            g_auto_rolled = 1;
            g_auto_t = now + 1000;
            return;
        }
        if (n != s.team_max) { lobby_auto_pick((unsigned)rnd(0x7FFFFFFF)); g_auto_t = now + 1500; return; }
        if (s.item_preset == PRESET_HOUSE) {   // dress this side's cats from this house
            int any = 0;
            for (int k = 0; k < n && !any; k++)
                for (int sl = 0; sl < NUM_SLOTS; sl++) if (mine[k].kit[sl][0]) { any = 1; break; }
            if (!any && !g_auto_dress_tried) { g_auto_dress_tried = 1; lobby_auto_kits(); g_auto_t = now + 1500; return; }
        }
        static unsigned long long announced;
        if (mode == 1 && g_remote_n == 0 && now - announced > 8000) {   // the client may have missed it
            announced = now;
            pvp_net_send(SUB_LOBBY_OPEN, NULL, 0);
        }
        if (mode == 1 && g_remote_n == s.team_max && s.item_preset == PRESET_HOUSE) {
            // The client dresses after it has picked; its kits arrive in a later PICK.
            static unsigned long long wait_from;
            int dressed = 0;
            for (int k = 0; k < g_remote_n && !dressed; k++)
                for (int sl = 0; sl < NUM_SLOTS; sl++) if (g_remote[k].kit[sl][0]) { dressed = 1; break; }
            if (!wait_from) wait_from = now;
            if (!dressed && now - wait_from < 10000) { g_auto_t = now + 1000; return; }
            wait_from = 0;
        }
        if (mode == 1 && g_remote_n == s.team_max) {
            char d[256];
            auto_describe(&s, d, sizeof d);
            alog("fight %d: new cats, %s", g_auto_fights + 1, d);
            host_start_from_lobby(mine, n, &s);
            g_auto_t = now + 5000;
        }
        return;
    }

    if (g_arena_active) {
        if (!g_auto_in_fight) {
            g_auto_in_fight = 1;
            g_auto_fight_t0 = now;
            g_auto_end_seen = 0;
            g_auto_fights++;
            alog("fight %d started (%s)", g_auto_fights, mode == 1 ? "host" : "client");
        }
        if (g_end_at && !g_auto_end_seen) {
            g_auto_end_seen = now;
            alog("fight %d over after %llu s", g_auto_fights, (now - g_auto_fight_t0) / 1000);
        }
        if (mode != 1) return;
        int timed_out = !g_end_at && now - g_auto_fight_t0 > (unsigned long long)g_auto_fight_minutes * 60000ULL;
        if ((g_auto_end_seen && now - g_auto_end_seen > 4000) || timed_out) {
            if (timed_out) alog("fight %d hit the %d min limit", g_auto_fights, g_auto_fight_minutes);
            g_auto_in_fight = 0;
            if (++g_auto_since_home < g_auto_home_every) {
                auto_roll(&g_fight_settings, 1);
                char d[256];
                auto_describe(&g_fight_settings, d, sizeof d);
                alog("fight %d: rematch, %s", g_auto_fights + 1, d);
                host_start(1);
            } else {
                g_auto_since_home = 0;
                alog("going home for new cats");
                net_end_choice(0);
            }
            g_auto_t = now + 6000;
        }
        return;
    }
    g_auto_in_fight = 0;

    // The house: the host opens the lobby (and re-announces it above if the client
    // missed the first one); a client that arrives in the house later opens its
    // own, which asks the host to open one too.
    if (mode == 2 && find_house_scene() && !lobby_is_open()) {
        g_auto_dress_tried = 0;
        open_lobby();
        g_auto_t = now + 5000;
        return;
    }
    if (mode == 1 && find_house_scene() && !lobby_is_open()) {
        for (int i = 0; i < g_remote_n; i++) { free(g_remote_bytes[i]); g_remote_bytes[i] = NULL; }
        g_remote_n = 0;   // last lobby's picks must not start this one
        g_auto_rolled = 0;
        g_auto_dress_tried = 0;
        open_lobby();
        g_auto_t = now + 3000;
    }
}

// Auto mode, getting in: the game boots to the main menu. Play is pressed through
// Button::Click (RVA 0x97E8E0, force 0 -- what Button::update itself does; the
// button's name is the std::string at Button+504), and on the save screen slot 0
// is continued through SaveSelection::ContinueSlot (RVA 0x1BD8B0) from a hook on
// SaveSelection::update (RVA 0x1BB780), installed only in auto mode.
#define RVA_BUTTON_CLICK   0x97E8E0
#define RVA_SAVESEL_UPDATE 0x1BB780
#define RVA_SAVESEL_SLOT   0x1BD8B0
#define OFF_BUTTON_NAME    504
static const unsigned char kSaveSelUpdateEntry[11] = { 0x48,0x8B,0xC4, 0x55, 0x53, 0x56, 0x57, 0x41,0x54, 0x41,0x55 };
typedef void (__fastcall *fn_savesel_update)(void*);
typedef void (__fastcall *fn_savesel_slot)(void*, int, unsigned char);
typedef void (__fastcall *fn_button_click)(void*, unsigned char);
static fn_savesel_update  g_orig_savesel_update;
static unsigned long long g_auto_click_at, g_auto_savesel_seen;

static void auto_button(unsigned char* btn) {
    if (!g_auto || !btn) return;
    unsigned long long now = GetTickCount64();
    if (now < g_auto_click_at) return;
    char name[64];
    __try {
        if (!read_std_string(btn + OFF_BUTTON_NAME, name, sizeof name)) return;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (strcmp(name, "MainMenu_Button_Play") != 0) return;
    g_auto_click_at = now + 4000;
    alog("main menu: pressing Play");
    __try {
        ((fn_button_click)((unsigned char*)GetModuleHandleA(NULL) + RVA_BUTTON_CLICK))(btn, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { alog("!! fault pressing Play"); }
}

static void __fastcall hook_savesel_update(void* ss) {
    g_orig_savesel_update(ss);
    unsigned long long now = GetTickCount64();
    if (!g_auto_savesel_seen || now - g_auto_savesel_seen > 30000) { g_auto_savesel_seen = now; return; }
    if (now - g_auto_savesel_seen < 1500) return;   // let the screen settle first
    g_auto_savesel_seen = now + 30000;              // once per visit
    alog("save screen: continuing slot 0");
    __try {
        ((fn_savesel_slot)((unsigned char*)GetModuleHandleA(NULL) + RVA_SAVESEL_SLOT))(ss, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { alog("!! fault continuing slot 0"); }
}

static void auto_hooks(void) {
    if (!g_auto) return;
    hook(RVA_SAVESEL_UPDATE, kSaveSelUpdateEntry, 11, (void*)hook_savesel_update, (void**)&g_orig_savesel_update,
         "SaveSelection::update (auto mode)");
}

// The game's opening cutscene ("game_intro", started by GameBase::init unless the
// settings say dev_mode) runs ~a minute and cannot be avoided at boot. With
// pvp.ini [probe] SkipIntro=1 (the default) it plays fast instead: a hook on
// SwfCutscene::update (vtable slot 10, RVA 0x829A00) finds the cutscene by its
// name (the std::string at +0x140 that the teardown turns into "cutscenes/<name>")
// and sets its entity's time scale (Entity+0x10, the double every component's dt
// is multiplied by -- the same field the winner banner freeze uses) to 25. The
// cutscene then ends by itself and the game moves on exactly as if it had been
// watched. Other cutscenes are left alone.
#define RVA_CUTSCENE_UPDATE 0x829A00
#define OFF_CUTSCENE_NAME   0x140
#define OFF_COMP_ENTITY     0x18
static const unsigned char kCutsceneUpdateEntry[10] = { 0x48,0x8B,0xC4, 0x48,0x89,0x58,0x20, 0x55, 0x56, 0x57 };
typedef void (__fastcall *fn_cutscene_update)(void*);
static fn_cutscene_update g_orig_cutscene_update;
static int g_skip_intro = 1;

static void __fastcall hook_cutscene_update(void* cs) {
    __try {
        char name[48];
        if (g_skip_intro && read_std_string((unsigned char*)cs + OFF_CUTSCENE_NAME, name, sizeof name) &&
            strcmp(name, "game_intro") == 0) {
            unsigned char* ent = *(unsigned char**)((unsigned char*)cs + OFF_COMP_ENTITY);
            if (ent && *(double*)(ent + OFF_ENTITY_TIMESCALE) != 25.0) {
                *(double*)(ent + OFF_ENTITY_TIMESCALE) = 25.0;
                plog("intro: playing the opening cutscene at 25x");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_orig_cutscene_update(cs);
}

static void intro_hooks(HMODULE self) {
    char ini[MAX_PATH];
    pvp_ini_path(self, ini);
    g_skip_intro = GetPrivateProfileIntA("probe", "SkipIntro", 1, ini);
    if (g_skip_intro)
        hook(RVA_CUTSCENE_UPDATE, kCutsceneUpdateEntry, 10, (void*)hook_cutscene_update,
             (void**)&g_orig_cutscene_update, "SwfCutscene::update (fast intro)");
}

// Run 59: Character::Face (RVA 0x10CE50) does not only write the facing -- it
// queues deferred lambdas (its lambda_7..12) that turn the cat again later. The
// AI's endturn path calls it through "orient" (FUN_14013C460) inside GetChoice, on
// the deciding peer only, so mgmp's state fence put the facing back and the lambda
// turned it again a moment later. While mgmp is inside a human cat's GetChoice
// (pvp_choice_fence), Face does nothing at all.
#define RVA_CHAR_FACE 0x10CE50
static const unsigned char kFaceEntry[15] = { 0x48,0x89,0x5C,0x24,0x20, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41 };
typedef void (__fastcall *fn_face)(void*, unsigned long long, char, char);
static fn_face g_orig_face;
static int     g_choice_fence;
static long    g_face_swallowed;

void pvp_choice_fence(int on) { g_choice_fence += on ? 1 : -1; if (g_choice_fence < 0) g_choice_fence = 0; }

static void __fastcall hook_face(void* ch, unsigned long long dir, char a, char b) {
    if (g_choice_fence && g_arena_online) {
        if (g_face_swallowed++ < 3) plog("net: Character::Face inside a GetChoice swallowed (%ld so far)", g_face_swallowed);
        return;
    }
    g_orig_face(ch, dir, a, b);
}

static void face_hooks(void) {
    hook(RVA_CHAR_FACE, kFaceEntry, 15, (void*)hook_face, (void**)&g_orig_face, "Character::Face (choice fence)");
}

// Soak run 2026-10-26, session 12: wp_MoneyBag ("X_is random_0_to_N") scattered a
// different number of coins on each peer, and the sim streams parted. An ability's
// X lives at Ability+0x6C4 and FUN_140044450 re-rolls it -- mode 16 (+0x204) draws
// randint on the SIMULATION stream -- whenever the ability is selected or previewed,
// i.e. on the aiming peer only. So during an online fight: no re-roll while the
// game is asking a human cat for a decision or previewing it (pvp_choice_fence,
// also held across PlayerBrain::update), and one roll right before
// Ability::trigger instead (pvp_before_trigger, from mgmp's trigger hook) -- the
// same instant of the simulation on both peers.
#define RVA_ABILITY_ROLL_X  0x44450
#define OFF_ABILITY_X_MODE  0x204
static const unsigned char kRollXEntry[15] = { 0x48,0x89,0x5C,0x24,0x08, 0x48,0x89,0x6C,0x24,0x10, 0x48,0x89,0x74,0x24,0x18 };
typedef void (__fastcall *fn_roll_x)(void*);
static fn_roll_x g_orig_roll_x;

static void __fastcall hook_roll_x(void* ability) {
    if (g_choice_fence && g_arena_online) return;
    g_orig_roll_x(ability);
}

void pvp_before_trigger(void* ability) {
    if (!g_arena_online || !g_arena_active || !g_orig_roll_x || !ability) return;
    __try {
        if (*(int*)((unsigned char*)ability + OFF_ABILITY_X_MODE) == 16) g_orig_roll_x(ability);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void roll_x_hooks(void) {
    hook(RVA_ABILITY_ROLL_X, kRollXEntry, 15, (void*)hook_roll_x, (void**)&g_orig_roll_x, "Ability X roll (choice fence)");
}
