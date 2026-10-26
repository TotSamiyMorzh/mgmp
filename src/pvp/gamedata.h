// gamedata.h -- the game text and item lists the arena needs, read at runtime from
// the player's own resources.gpak (nothing from the game is compiled into the DLL).
//
// What used to be generated offline by pvp/tools/make_item_text.py,
// make_skill_text.py and make_item_table.py is built here, by the same rules, on a
// background thread started at load: item names / descriptions / rarity / flat
// stats / icon frame, class names, ability and passive names (Russian and English,
// from data/text/combined.csv), and the per-slot pool the random presets draw from.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ITEM_TEXT_STRUCTS
#define ITEM_TEXT_STRUCTS
struct item_text { const char* id; const char* name; const char* desc; int rarity; const char* stats; int frame; };
struct class_text { const char* id_upper; const char* name; };
#endif
#ifndef SKILL_TEXT_STRUCTS
#define SKILL_TEXT_STRUCTS
struct skill_text { const char* id; const char* name; const char* desc; const char* base; };
#endif

typedef void (*gd_log_fn)(const char* fmt, ...);

// Starts loading on a background thread (once). Safe to call again.
void gd_start(gd_log_fn log);
// 1 once everything below is available; until then the lookups return nothing.
int  gd_ready(void);

// The random presets' pool: regular items of a slot (0 head .. 4 trinket),
// sorted, identical on every machine with the same game data.
int         gd_slot_count(int slot);
const char* gd_slot_item(int slot, int i);

// Tables sorted by id (strcmp), for binary search. `en`: 1 English, 0 Russian.
const struct item_text*  gd_items(int en, int* n);
const struct class_text* gd_classes(int en, int* n);
const struct skill_text* gd_abilities(int en, int* n);
const struct skill_text* gd_passives(int en, int* n);

// For the item icons: the item's kind ("head", "face", "neck", "weapon",
// "trinket") and its frame in swfs/catparts.swf (1-based; 0 when none).
const char* gd_item_kind(const char* id);
int         gd_item_frame(const char* id);

#ifdef __cplusplus
}
#endif
