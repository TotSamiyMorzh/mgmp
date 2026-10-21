// pvp_clone -- the arena fights CLONES of the chosen cats, never the cats themselves.
//
// A clone is a fresh CatData built the way the game builds one on a cache miss in
// CatData::by_id (FUN_1400D7220): operator new(0xC58), zeroed, FUN_14005D6B0, then
// filled from bytes by the game's own bidirectional SerializeCatData -- the bytes the
// owner's machine wrote from the real cat (mgmp_catsync's serialize path).
//
// Clones get keys (CatData+0xC48, what the run list and by_id use) in a range no save
// can hold, and a hook on CatData::by_id answers those keys with the clone. Nothing is
// ever inserted into the game's cat registry, so nothing can reach the save; the real
// cats are not touched at all, which makes the friendly mode free -- and both peers of
// an online fight can build byte-identical clones of all the fight's cats.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PVP_CLONE_MAX      16
#define PVP_CLONE_KEY_BASE 0x7A70000000000000LL   // + slot

// Hooks CatData::by_id (from pvp_init). Returns 1 when live.
int   pvp_clone_init(void);
// Serializes a real cat; *out is malloc'ed (free() it). Returns the size, 0 on failure.
uint32_t pvp_clone_serialize(void* cat, uint8_t** out);
// Builds (or rebuilds) clone `slot` from serialized bytes. Returns the CatData*, or NULL.
void* pvp_clone_from_bytes(int slot, const uint8_t* data, uint32_t size);
// The clone key for a slot, and the clone behind a key (NULL when none).
long long pvp_clone_key(int slot);
void* pvp_clone_by_key(long long key);
// Forget all clones' keys (by_id stops answering them); the objects are kept for reuse.
void  pvp_clone_clear(void);

#ifdef __cplusplus
}
#endif
