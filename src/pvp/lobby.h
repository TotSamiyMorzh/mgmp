// lobby -- the arena's settings window, an ImGui overlay drawn from the probe's
// SDL_GL_SwapWindow detour. C interface; the implementation is C++ (lobby.cpp).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define LOBBY_MAX_CATS 64
#define LOBBY_MAX_ITEMS 512
#define LOBBY_SLOTS 5           // head face neck weapon trinket (CatData order)

// One item in the house storage. The arena never takes it out: a picked item is
// built anew by name for the fight, so the storage only marks it "taken" in the UI.
typedef struct {
    long long id;           // Equipment id
    char      name[48];     // item id in the game data, e.g. "UraniumRod"
    int       slot;         // 0..4, from the game
} lobby_item;

typedef struct {
    void*     cat;          // CatData*
    long long key;          // sql_key
    char      name[64];     // UTF-8
    char      room[48];
    int       team;         // 0 = not playing, 1 / 2 = player
    // Shown in the list (owner, run 29): to tell cats apart by more than a name.
    int       sex;          // 0 male, 1 female, 2 other (CatData+0x58)
    int       level;        // CatData+0xC30
    int       age_days;     // current day (MewDirector+0x580) - birthday (CatData+0xC38)
    char      cls[24];      // collar / class (CatData+0xC10), "" when none
    int       stats[7];     // str dex con int spd cha lck: base + levelling + injuries
    int       pick;         // order the cat joined its team in; the lobby keeps it
    // House preset: the kit the player picked from the storage ("" / 0 = empty).
    char      kit[LOBBY_SLOTS][48];
    long long kit_id[LOBBY_SLOTS];
    // Face parts for the portrait (CatData BodyParts @+0x60): head ear eye brow mouth
    // texture palette; head 0 = no portrait.
    int       look[7];
    // Active abilities usable in battle (CatData actives_accessible[4] @+0x810) and
    // passives (passive_0 @+0x910, passive_1 @+0x938); "" = empty.
    char      abil[4][40];
    char      passive[2][40];
} lobby_cat;

// Item presets use the probe's numbering: 0 none, 1 random, 2 house, 3 balanced.
typedef struct {
    char player_name[2][32];
    int  item_preset;
    int  items_per_cat;
    int  house_mirror;
    int  money_on;
    int  money;
    int  team_max;          // cats per side, 1..4
    int  cover_count;       // obstacles on the arena, 0..16, placed in mirrored pairs
    int  cover_kind;        // 0 mixed, 1 breakable, 2 solid
    int  loot_count;        // pickups on the ground, 0..16, mirrored pairs
    int  loot_mask;         // bit 0 coins, 1 catnip, 2 food, 3 scrap
    int  mob_count;         // monsters, 0..8, mirrored pairs
    int  mob_kind;          // 0 weak, 1 medium, 2 mixed
    int  mob_mode;          // 0 against both players, 1 every one for itself
    int  mob_mad;           // monsters get Madness (attack anyone)
    int  bird_count;        // birds that give the killer an item, 0..6
    int  bird_mask;         // bit 0 small, 1 medium, 2 large
    int  bird_every;        // a wave of birds every N rounds, 0 = never
    int  bird_wave;         // waves top the living birds up to this many, 1..3
} lobby_settings;

typedef void (*lobby_log_fn)(const char* fmt, ...);

void lobby_set_log(lobby_log_fn f);
// Opens the window with the house's cats (team preassigned) and the defaults.
void lobby_open(const lobby_cat* cats, int n, const lobby_settings* s);
// The house storage, for the house preset's picking step. Call before lobby_open.
void lobby_set_storage(const lobby_item* items, int n);
int  lobby_is_open(void);
// Call once per frame, on the game thread, with the GL context current.
void lobby_on_swap(void);
// Returns 1 once after the players pressed СТАРТ, filling the chosen cats
// (with their teams) and settings.
int  lobby_take_start(lobby_cat* cats, int* n, lobby_settings* s);

#ifdef __cplusplus
}
#endif
