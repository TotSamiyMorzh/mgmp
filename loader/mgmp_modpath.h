// mgmp_modpath.h -- the PvP arena's -modpaths data, made from the player's own
// game files by the loader before the game starts (see mgmp_modpath.cpp).
#pragma once
#include <string>

// Writes (or keeps, when its stamp still matches the game's resources.gpak) the
// arena level and the PvP bird data under `modpath`. `report` says what happened.
bool modpath_generate(const std::wstring& game_exe, const std::wstring& modpath, std::wstring& report);
