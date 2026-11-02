#pragma once
#include <windows.h>
#include <stdint.h>
#include "lobby.h"
#ifdef __cplusplus
extern "C" {
#endif
void pvp_test_init(HMODULE self);
int pvp_test_enabled(void);
int pvp_test_manual_face(void* brain);
uint64_t pvp_test_begin(void);
int pvp_test_cat(uint8_t** bytes, uint32_t* size, lobby_cat* row, int slot);
void pvp_test_capture(const void* data, uint32_t size);
uint32_t pvp_test_replay(uint8_t** bytes);
void pvp_test_trigger(void* ability);
void pvp_test_trigger_end(void* ability);
void pvp_test_spawn(void* character, void* type_name);
void pvp_test_choice_scope(int on);
uint64_t pvp_test_rng_hash(void);
void pvp_test_roll_x(void* ability, uintptr_t caller, uint64_t before);
#ifdef __cplusplus
}
#endif
