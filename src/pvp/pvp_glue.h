// pvp_glue.h -- what mgmp calls in the arena module (src/pvp).
#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// Once, from mgmp's init thread after hooks_install(): installs the arena's hooks.
void pvp_init(HMODULE self);
// Every frame, from mgmp's SDL swap detour (before the debug panel draws).
void pvp_frame(void);
// After every Button::update, from mgmp's h_ButtonUpdate.
void pvp_button_update(void* button);
// Around StatusMenu::update (the battle HUD / portrait panel), from mgmp's h_StatusMenu.
void pvp_status_menu_begin(void* panel);
void pvp_status_menu_end(void);

#ifdef __cplusplus
}
#endif
