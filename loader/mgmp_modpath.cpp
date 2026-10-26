// mgmp_modpath.cpp -- writes the PvP arena's data overrides into the -modpaths
// folder, from the player's own resources.gpak, before the game starts.
//
// The game reads loose files from -modpaths ahead of the archive, and it reads
// them at startup -- before mgmp.dll is injected -- so the loader makes them. They
// are derived from the player's own game files and never ship with the mod:
//   levels/pvp/a.lvl                  levels/house_bosses/terminator3.lvl with only
//                                     the cat spawns kept, plus each one mirrored
//                                     through the map centre (team 2)
//   data/spawns.gon                   the game's file + spawn ids 9101..9103
//   data/characters/bonus_birds.gon   the game's file + the PvP birds
// (The C++ twin of pvp/tools/make_arena.py --mirror and make_pvp_data.py.)
// A stamp holds the archive's size and time, so a game update regenerates them.

#include "mgmp_modpath.h"
#include "../src/pvp/portrait/gpak.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kGeneratorVersion = 1;

const char kBirds[] =
"\n\n// ---- PvP arena (mgmp_loader) ----\n"
"PvpBirdBase {\n"
"    graphics {\n"
"        movieclip BirdSmall\n"
"        name \"ENEMY_BIRD_NAME\"\n"
"        tooltip \"ENEMY_BIRD_DESC\"\n"
"    }\n"
"\n"
"    properties {\n"
"        tag animal\n"
"        hidden_tag [bird]\n"
"        faction third_party\n"
"        kill_required false\n"
"        movement 3\n"
"        health 2\n"
"        initiative -999\n"
"    }\n"
"\n"
"    abilities {\n"
"        move DefaultMove\n"
"        attack BasicMelee\n"
"        spells [\n"
"            BirdFly\n"
"        ]\n"
"    }\n"
"\n"
"    passives {\n"
"        CounterAttack attack\n"
"        MoveAwayFromDamageSource {\n"
"            move_ability BirdFly\n"
"        }\n"
"    }\n"
"\n"
"    ai {\n"
"        brain GenericBrain\n"
"        decision_weights simple\n"
"        move_weights bird\n"
"    }\n"
"}\n"
"\n"
"PvpBirdSmall {\n"
"    variant_of PvpBirdBase\n"
"    graphics {\n"
"        movieclip BirdSmall\n"
"        name \"ENEMY_LITTLEBIRD_NAME\"\n"
"        shadow_size .5\n"
"    }\n"
"    properties {\n"
"        health 2\n"
"        movement 5\n"
"    }\n"
"    passives {\n"
"        BirdRewards {\n"
"            statuses {\n"
"                AllStatsUp 1\n"
"            }\n"
"            ally_rewards {\n"
"                FindItemFromPool chapter\n"
"            }\n"
"        }\n"
"        RunInXTurns 3\n"
"    }\n"
"}\n"
"\n"
"PvpBirdMed {\n"
"    variant_of PvpBirdBase\n"
"    graphics {\n"
"        movieclip BirdMed\n"
"        name \"ENEMY_MEDBIRD_NAME\"\n"
"    }\n"
"    properties {\n"
"        health 7\n"
"        movement 4\n"
"    }\n"
"    passives {\n"
"        BirdRewards {\n"
"            statuses {\n"
"                AllStatsUp 1\n"
"            }\n"
"            ally_rewards {\n"
"                FindItemFromPool chapter\n"
"            }\n"
"        }\n"
"        RunInXTurns 4\n"
"    }\n"
"}\n"
"\n"
"PvpBirdLarge {\n"
"    variant_of PvpBirdBase\n"
"    graphics {\n"
"        movieclip BirdLarge\n"
"        name \"ENEMY_BIGBIRD_NAME\"\n"
"    }\n"
"    properties {\n"
"        health 16\n"
"        movement 3\n"
"    }\n"
"    passives {\n"
"        BirdRewards {\n"
"            statuses {\n"
"                AllStatsUp 1\n"
"            }\n"
"            ally_rewards {\n"
"                FindItemFromPool chapter\n"
"            }\n"
"        }\n"
"        RunInXTurns 5\n"
"    }\n"
"}\n";

const char kSpawns[] =
"\n\n// ---- PvP arena (mgmp_loader) ----\n"
"9101 {\n"
"    editor {\n"
"        category 4\n"
"        name \"PvP Small Bird\"\n"
"        paint false\n"
"    }\n"
"    object PvpBirdSmall\n"
"}\n"
"\n"
"9102 {\n"
"    editor {\n"
"        category 4\n"
"        name \"PvP Medium Bird\"\n"
"        paint false\n"
"    }\n"
"    object PvpBirdMed\n"
"}\n"
"\n"
"9103 {\n"
"    editor {\n"
"        category 4\n"
"        name \"PvP Large Bird\"\n"
"        paint false\n"
"    }\n"
"    object PvpBirdLarge\n"
"}\n";

std::string utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool make_dirs(const std::wstring& path) {
    // Creates every missing directory of `path` (a directory).
    for (size_t i = 3; i <= path.size(); ++i)
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') {
            std::wstring part = path.substr(0, i);
            CreateDirectoryW(part.c_str(), nullptr);
        }
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool write_file(const std::wstring& dir, const std::wstring& rel, const void* data, size_t n) {
    std::wstring path = dir + L"\\" + rel;
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && !make_dirs(path.substr(0, slash))) return false;
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

bool file_exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string stamp_of(const std::wstring& gpak) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(gpak.c_str(), GetFileExInfoStandard, &a)) return std::string();
    char b[128];
    snprintf(b, sizeof b, "v%d size %lu:%lu time %lu:%lu\n", kGeneratorVersion,
             a.nFileSizeHigh, a.nFileSizeLow, a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
    return b;
}

// make_arena.py --mirror: keep the level, keep only the cat spawns (id 1), and add
// each one mirrored through the map centre for the second team.
bool make_arena(const std::vector<uint8_t>& d, std::vector<uint8_t>& out) {
    if (d.size() < 24) return false;
    uint32_t n = 0, w = 0, h = 0;
    memcpy(&n, &d[16], 4);
    memcpy(&w, &d[4], 4);
    memcpy(&h, &d[8], 4);
    size_t end = d.size() - 4;
    if ((size_t)n * 8 > end) return false;
    size_t start = end - (size_t)n * 8;
    if (d[end] || d[end + 1] || d[end + 2] || d[end + 3]) return false;   // the 4-byte zero trailer
    struct Rec { uint16_t x, y; uint32_t id; };
    std::vector<Rec> keep;
    for (uint32_t i = 0; i < n; ++i) {
        Rec r;
        memcpy(&r.x, &d[start + 8 * i], 2);
        memcpy(&r.y, &d[start + 8 * i + 2], 2);
        memcpy(&r.id, &d[start + 8 * i + 4], 4);
        if (r.id == 1) keep.push_back(r);
    }
    std::set<std::pair<int, int>> taken;
    for (auto& r : keep) taken.insert({ r.x, r.y });
    std::vector<Rec> orig = keep;
    for (auto& r : orig) {
        int mx = (int)w - 1 - r.x, my = (int)h - 1 - r.y;
        if (!taken.count({ mx, my })) keep.push_back({ (uint16_t)mx, (uint16_t)my, 1 });
    }
    out.assign(d.begin(), d.begin() + start);
    uint32_t cnt = (uint32_t)keep.size();
    memcpy(&out[16], &cnt, 4);
    for (auto& r : keep) {
        uint8_t b[8];
        memcpy(b, &r.x, 2); memcpy(b + 2, &r.y, 2); memcpy(b + 4, &r.id, 4);
        out.insert(out.end(), b, b + 8);
    }
    out.insert(out.end(), d.begin() + end, d.end());
    return true;
}

} // namespace

bool modpath_generate(const std::wstring& game_exe, const std::wstring& modpath, std::wstring& report) {
    std::wstring gpak = game_exe;
    size_t slash = gpak.find_last_of(L"\\/");
    gpak = (slash == std::wstring::npos ? std::wstring() : gpak.substr(0, slash + 1)) + L"resources.gpak";

    const std::string stamp = stamp_of(gpak);
    if (stamp.empty()) { report = L"no resources.gpak next to the game"; return false; }
    const std::wstring stamp_path = modpath + L"\\mgmp_stamp.txt";
    bool fresh = file_exists(modpath + L"\\levels\\pvp\\a.lvl") &&
                 file_exists(modpath + L"\\data\\spawns.gon") &&
                 file_exists(modpath + L"\\data\\characters\\bonus_birds.gon");
    if (fresh) {
        std::string old;
        if (FILE* f = _wfopen(stamp_path.c_str(), L"rb")) {
            char b[256] = {};
            size_t n = fread(b, 1, sizeof b - 1, f);
            old.assign(b, n);
            fclose(f);
        }
        if (old == stamp) { report = L"up to date"; return true; }
    }

    cr::GPak gp;
    if (!gp.open(utf8(gpak))) { report = L"cannot read resources.gpak"; return false; }
    std::vector<uint8_t> lvl, arena, spawns, birds;
    if (!gp.read("levels/house_bosses/terminator3.lvl", lvl) || !make_arena(lvl, arena)) {
        report = L"cannot build the arena level from levels/house_bosses/terminator3.lvl";
        return false;
    }
    if (!gp.read("data/spawns.gon", spawns) || !gp.read("data/characters/bonus_birds.gon", birds)) {
        report = L"cannot read data/spawns.gon or data/characters/bonus_birds.gon";
        return false;
    }
    spawns.insert(spawns.end(), kSpawns, kSpawns + sizeof kSpawns - 1);
    birds.insert(birds.end(), kBirds, kBirds + sizeof kBirds - 1);
    bool ok = write_file(modpath, L"levels\\pvp\\a.lvl", arena.data(), arena.size()) &&
              write_file(modpath, L"data\\spawns.gon", spawns.data(), spawns.size()) &&
              write_file(modpath, L"data\\characters\\bonus_birds.gon", birds.data(), birds.size()) &&
              write_file(modpath, L"mgmp_stamp.txt", stamp.data(), stamp.size());
    report = ok ? L"written from this game's files" : L"could not write into the modpath folder";
    return ok;
}
