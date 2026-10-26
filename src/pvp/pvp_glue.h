// pvp_glue.h -- the seams between mgmp and the arena module (src/pvp).
#pragma once
#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- what mgmp calls in the arena (implemented in pvp_arena.c) --------------
// Once, from mgmp's init thread after hooks_install(): installs the arena's hooks.
void pvp_init(HMODULE self);
// Every frame, from mgmp's SDL swap detour (before the debug panel draws).
void pvp_frame(void);
// After every Button::update, from mgmp's h_ButtonUpdate.
void pvp_button_update(void* button);
// Around StatusMenu::update (the battle HUD / portrait panel), from mgmp's h_StatusMenu.
void pvp_status_menu_begin(void* panel);
void pvp_status_menu_end(void);
// A MSG_PVP arrived (from mgmp's lockstep_pump, game thread). `data` is freed after.
void pvp_on_message(uint8_t from, uint8_t sub, const uint8_t* data, uint32_t size);
// Auto mode (pvp.ini [auto] Enabled=1) during an online fight: mgmp's GetChoice
// hook lets the game's AI decide for THIS peer's own cats (soak testing).
int  pvp_auto_on(void);
// Around a human cat's GetChoice (mgmp's hook): Character::Face is swallowed
// while on, so a decision derived there cannot turn the cat (see pvp_arena.c).
void pvp_choice_fence(int on);
// Right before Ability::trigger during an online fight (mgmp's trigger hook):
// re-rolls a random X ability's X on the sim stream, on both peers alike.
void pvp_before_trigger(void* ability);

// --- what the arena may ask of mgmp (implemented in pvp_glue.cpp) -----------
// 0 = no PvP session (hotseat), 1 = PvP host, 2 = PvP client. A session counts once
// the handshake is complete and mgmp.json says net.mode = "pvp".
int  pvp_net_mode(void);
// Send one arena message to the other player. Returns 1 when it went out.
int  pvp_net_send(uint8_t sub, const void* data, uint32_t size);
// Lockstep: the fight's identity, and who owns which cat (peer position 0 = host).
void pvp_lockstep_enter(uint64_t battle_id);
void pvp_lockstep_split(int (*owner)(const void* character));
// 1 once lockstep halted on a desync (both peers freeze).
int  pvp_lockstep_halted(void);

#ifdef __cplusplus
}
#endif
