// lobby.cpp -- the arena lobby, an ImGui overlay.
//
// Rendering follows mgmp's debug panel (mgmp/src/ui/mgmp_ui.cpp): ImGui with the
// Win32 + OpenGL3 backends, brought up on the first frame that has a current GL
// context (the font atlas is a texture), drawn from the SDL_GL_SwapWindow detour,
// input taken by subclassing the game window. GLSL 150: the game asks for a 3.2
// core context. The game hides the OS cursor and draws its own, so ImGui draws
// its own too while the lobby is open.
//
// While open, the lobby takes every mouse and keyboard message, so a click on it
// never reaches the house behind it.

#include "lobby.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_opengl3.h"
#include "gamedata.h"
#include <GL/gl.h>
// stb_image's implementation is in mgmp_overlay.cpp (built without stdio).
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "portraits.h"

// The lobby follows the game language: English unless the game is in Russian
// (lobby_set_english, from the arena at startup). Decided once, before the first
// draw, so static tables built from TR() are built in the right language.
static int g_lobby_en;
#define TR(ru, en) (g_lobby_en ? (en) : (ru))

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

typedef HDC(__stdcall* fn_wglGetCurrentDC)(void);

struct Lobby {
    bool    open = false;
    bool    ready = false;
    bool    failed = false;
    bool    start_pending = false;
    HWND    hwnd = nullptr;
    WNDPROC prev_wndproc = nullptr;
    // mgmp.dll: mgmp's debug panel owns the global ImGui context, so the lobby
    // keeps its own and switches to it (and back) at every entry point.
    ImGuiContext* ctx = nullptr;

    // Online (net.mode = "pvp"): this player picks for my_team only; the other team
    // is what the other player picked (remote). The client cannot edit the rules.
    bool      online = false, is_host = false;
    int       my_team = 0;
    char      remote_name[32] = {};
    lobby_cat remote[8];
    int       nremote = 0;
    bool      cancelled = false;
    lobby_log_fn log = nullptr;

    lobby_cat      cats[LOBBY_MAX_CATS];
    int            ncats = 0;
    lobby_settings s{};
    int            pick_seq = 0;

    lobby_item     items[LOBBY_MAX_ITEMS];
    int            nitems = 0;
    int            page = 0;          // 0 the lobby, 1 / 2 that player's item picking
    int            sel_cat = -1, sel_slot = 0;
    GLuint         icons = 0;         // unused (item icons come from the portrait worker)
};
Lobby g;

#define LOG(...) do { if (g.log) g.log(__VA_ARGS__); } while (0)

HWND game_window() {
    HMODULE ogl = GetModuleHandleA("opengl32.dll");
    if (!ogl) return nullptr;
    auto get_dc = (fn_wglGetCurrentDC)GetProcAddress(ogl, "wglGetCurrentDC");
    HDC dc = get_dc ? get_dc() : nullptr;
    return dc ? WindowFromDC(dc) : nullptr;
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g.ready && g.open) {
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { g.open = false; g.cancelled = true; return 0; }
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g.ctx);
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        ImGui::SetCurrentContext(prev);
        switch (msg) {   // the lobby is modal: the house sees none of this
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN:
            case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP:
                return 0;
            default: break;
        }
    }
    return CallWindowProcW(g.prev_wndproc, hwnd, msg, wp, lp);
}

// Paper-ish palette, closer to the game's menus than ImGui's dark default.
void style() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsLight();
    st.WindowRounding = 6.0f; st.FrameRounding = 4.0f; st.ChildRounding = 6.0f;
    st.WindowPadding = ImVec2(18, 14); st.FramePadding = ImVec2(10, 6); st.ItemSpacing = ImVec2(10, 8);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]       = ImVec4(0.93f, 0.92f, 0.89f, 0.98f);
    c[ImGuiCol_ChildBg]        = ImVec4(0.88f, 0.87f, 0.83f, 1.00f);
    c[ImGuiCol_Border]         = ImVec4(0.25f, 0.24f, 0.22f, 0.60f);
    c[ImGuiCol_Text]           = ImVec4(0.12f, 0.11f, 0.10f, 1.00f);
    c[ImGuiCol_FrameBg]        = ImVec4(0.98f, 0.97f, 0.95f, 1.00f);
    c[ImGuiCol_Button]         = ImVec4(0.82f, 0.80f, 0.76f, 1.00f);
    c[ImGuiCol_ButtonHovered]  = ImVec4(0.90f, 0.72f, 0.72f, 1.00f);
    c[ImGuiCol_ButtonActive]   = ImVec4(0.80f, 0.55f, 0.55f, 1.00f);
    c[ImGuiCol_Header]         = ImVec4(0.86f, 0.70f, 0.70f, 1.00f);
    c[ImGuiCol_HeaderHovered]  = ImVec4(0.90f, 0.75f, 0.75f, 1.00f);
    c[ImGuiCol_TitleBgActive]  = ImVec4(0.55f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_CheckMark]      = ImVec4(0.55f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_SliderGrab]     = ImVec4(0.55f, 0.20f, 0.20f, 1.00f);
}

// Item icons are rendered from the player's own swfs/catparts.swf by the portrait
// worker (draw_icon); nothing from the game ships with the mod.

bool bring_up() {
    g.hwnd = game_window();
    if (!g.hwnd) { LOG("lobby: no window behind the GL DC"); return false; }
    IMGUI_CHECKVERSION();
    g.ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g.ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    // Run 29: the game's own cursor stays visible over the lobby, so ImGui drawing
    // a second one gave a doubled, trailing pointer. Leave the cursor to the game.
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // Cyrillic needs a font that has it; ImGui's built-in one does not.
    char font[MAX_PATH];
    GetWindowsDirectoryA(font, MAX_PATH);
    strcat_s(font, "\\Fonts\\segoeui.ttf");
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) {
        static const ImWchar kRanges[] = { 0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x2116, 0x2116, 0x2190, 0x2193, 0 };
        io.Fonts->AddFontFromFileTTF(font, 22.0f, nullptr, kRanges);
        // ♂ ♀ live in Segoe UI Symbol; merged in as a fallback.
        char sym[MAX_PATH];
        GetWindowsDirectoryA(sym, MAX_PATH);
        strcat_s(sym, "\\Fonts\\seguisym.ttf");
        if (GetFileAttributesA(sym) != INVALID_FILE_ATTRIBUTES) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            static const ImWchar kSex[] = { 0x2640, 0x2642, 0 };
            io.Fonts->AddFontFromFileTTF(sym, 22.0f, &cfg, kSex);
        }
    } else {
        io.Fonts->AddFontDefault();
    }
    style();
    portraits_start(g.log);
    if (!ImGui_ImplWin32_Init(g.hwnd)) { ImGui::DestroyContext(g.ctx); g.ctx = nullptr; LOG("lobby: Win32 backend failed"); return false; }
    if (!ImGui_ImplOpenGL3_Init("#version 150")) {
        ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(g.ctx); g.ctx = nullptr; LOG("lobby: GL3 backend failed"); return false;
    }
    g.prev_wndproc = (WNDPROC)SetWindowLongPtrW(g.hwnd, GWLP_WNDPROC, (LONG_PTR)&wndproc);
    LOG("lobby: imgui %s up, hwnd %p, input %s", IMGUI_VERSION, (void*)g.hwnd, g.prev_wndproc ? "hooked" : "NOT hooked");
    return g.prev_wndproc != nullptr;
}

// Class by collar id, in the game's own translation (CAT_CLASS_<ID>_NAME, owner run
// 31: Necromancer showed in English). Collarless cats get a dash.
const char* class_name(const char* cls) {
    if (!cls[0] || !_stricmp(cls, "Colorless")) return "—";
    int n = 0;
    const class_text* tab = gd_classes(g_lobby_en, &n);
    for (int i = 0; i < n; i++) if (!_stricmp(cls, tab[i].id_upper)) return tab[i].name;
    return cls;
}

// House rooms (data/house.gon). The game never names them on screen, so these
// are ours.
const char* room_name(const char* room) {
    static const char* kMap[][2] = {
        { "Floor1_Large", TR("1 этаж, большая", "Floor 1, large") }, { "Floor1_Small", TR("1 этаж, малая", "Floor 1, small") },
        { "Floor2_Large", TR("2 этаж, большая", "Floor 2, large") }, { "Floor2_Small", TR("2 этаж, малая", "Floor 2, small") },
        { "Attic", TR("Чердак", "Attic") }, { "SmallAttic", TR("Чердак", "Attic") }, { "LargeAttic", TR("Чердак", "Attic") },
        { "Basement0", TR("Подвал", "Basement") }, { "Basement1", TR("Пещеры", "Caves") }, { "Basement2", TR("Глубины", "Depths") },
        { "Basement3", TR("Утроба", "Womb") }, { "Basement4", TR("Шеол", "Sheol") },
    };
    for (auto& m : kMap) if (!strcmp(room, m[0])) return m[1];
    return room;
}

// The cat's face (portraits.cpp), fitted into a sz x sz box at the cursor; a plain
// circle while it is still being rendered.
void cat_face(const lobby_cat& c, float sz) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    portrait_look l = { c.look[0], c.look[1], c.look[2], c.look[3], c.look[4], c.look[5], c.look[6] };
    int w = 0, h = 0;
    unsigned tex = portraits_tex(l, 128, &w, &h);
    if (tex && w > 0 && h > 0) {
        float s = sz / (float)(w > h ? w : h);
        float dw = w * s, dh = h * s;
        ImVec2 a(p.x + (sz - dw) / 2, p.y + (sz - dh) / 2);
        dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)tex), a, ImVec2(a.x + dw, a.y + dh));
    } else {
        dl->AddCircleFilled(ImVec2(p.x + sz / 2, p.y + sz / 2), sz * 0.38f, IM_COL32(205, 202, 194, 255));
    }
    ImGui::Dummy(ImVec2(sz, sz));
}

const skill_text* skill_info(const skill_text* tab, int n, const char* id) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(id, tab[mid].id);
        if (!c) return &tab[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return nullptr;
}

// A row of the cat's ability and passive icons; hover shows name and description.
void skill_icons(const lobby_cat& c, float sz) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool any = false;
    for (int k = 0; k < 6; k++) {
        int passive = k >= 4;
        const char* id = passive ? c.passive[k - 4] : c.abil[k];
        if (!id[0] || !_stricmp(id, "none")) continue;
        int nt = 0;
        const skill_text* tab = passive ? gd_passives(g_lobby_en, &nt) : gd_abilities(g_lobby_en, &nt);
        const skill_text* info = tab ? skill_info(tab, nt, id) : nullptr;
        if (any) ImGui::SameLine(0, 4);
        any = true;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(k);
        ImGui::InvisibleButton("sk", ImVec2(sz, sz));
        ImGui::PopID();
        unsigned tex = portraits_icon(passive, id, info ? info->base : nullptr, 64);
        if (passive)   // passives on a round plate, actives on a square one, like the game
            dl->AddCircleFilled(ImVec2(p.x + sz / 2, p.y + sz / 2), sz / 2, IM_COL32(205, 202, 194, 255));
        else
            dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), IM_COL32(205, 202, 194, 255), 4.0f);
        if (tex) dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)tex), p, ImVec2(p.x + sz, p.y + sz));
        else dl->AddText(ImVec2(p.x + 4, p.y + 4), IM_COL32(90, 86, 80, 255), passive ? TR("П", "P") : TR("С", "A"));
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
            ImGui::TextColored(passive ? ImVec4(0.18f, 0.38f, 0.78f, 1) : ImVec4(0.55f, 0.20f, 0.20f, 1), "%s",
                               info ? info->name : id);
            ImGui::TextDisabled(passive ? TR("пассивка", "passive") : TR("способность", "ability"));
            if (info && info->desc[0]) { ImGui::Separator(); ImGui::TextUnformatted(info->desc); }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (!any) ImGui::TextDisabled("—");
}

int team_count(int t) {
    if (g.online && t != g.my_team) return g.nremote;
    int n = 0;
    for (int i = 0; i < g.ncats; i++) n += g.cats[i].team == t;
    return n;
}

// Team t's cats in the order they were picked (owner, run 31: sorting the table
// must not shuffle the player cards). Returns the count.
int team_order(int t, int* idx) {
    int n = 0;
    for (int i = 0; i < g.ncats; i++)
        if (g.cats[i].team == t) {
            int j = n++;
            while (j > 0 && g.cats[idx[j - 1]].pick > g.cats[i].pick) { idx[j] = idx[j - 1]; j--; }
            idx[j] = i;
        }
    return n;
}

void player_card(int t) {
    static const ImU32 kColour[2] = { IM_COL32(70, 120, 190, 255), IM_COL32(190, 70, 70, 255) };
    ImGui::BeginChild(t == 1 ? "p1" : "p2", ImVec2(0, ImGui::GetIO().DisplaySize.y * 0.20f), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 34.0f;
    dl->AddCircleFilled(ImVec2(p.x + r, p.y + r), r, kColour[t - 1]);
    char initial[8] = "?";
    const char* nm = g.s.player_name[t - 1];
    if (nm[0]) {   // first UTF-8 character of the name
        int len = ((unsigned char)nm[0] < 0x80) ? 1 : ((unsigned char)nm[0] < 0xE0) ? 2 : 3;
        memcpy(initial, nm, len); initial[len] = 0;
    }
    ImVec2 ts = ImGui::CalcTextSize(initial);
    dl->AddText(ImVec2(p.x + r - ts.x / 2, p.y + r - ts.y / 2), IM_COL32_WHITE, initial);
    ImGui::Dummy(ImVec2(2 * r, 2 * r));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text(g.online ? (t == g.my_team ? TR("Игрок %d (вы)", "Player %d (you)") : TR("Игрок %d (соперник)", "Player %d (opponent)")) : TR("Игрок %d", "Player %d"), t);
    ImGui::SetNextItemWidth(-1);
    if (g.online && t != g.my_team) {
        ImGui::BeginDisabled();
        ImGui::InputText(t == 1 ? "##n1" : "##n2", g.remote_name, sizeof g.remote_name);
        ImGui::EndDisabled();
    } else {
        ImGui::InputText(t == 1 ? "##n1" : "##n2", g.s.player_name[t - 1], sizeof g.s.player_name[0]);
    }
    ImGui::Text(TR("Коты: %d / %d", "Cats: %d / %d"), team_count(t), g.s.team_max);
    ImGui::EndGroup();
    if (g.online && t != g.my_team) {   // the other player's picks
        for (int k = 0; k < g.nremote; k++) {
            cat_face(g.remote[k], 50);
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 13);
            ImGui::TextUnformatted(g.remote[k].name);
        }
        if (!g.nremote) ImGui::TextDisabled(TR("соперник ещё не выбрал котов", "the opponent has not picked cats yet"));
        ImGui::EndChild();
        return;
    }
    int idx[LOBBY_MAX_CATS], n = team_order(t, idx);
    for (int k = 0; k < n; k++) {
        cat_face(g.cats[idx[k]], 50);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 13);
        ImGui::TextUnformatted(g.cats[idx[k]].name);
    }
    ImGui::EndChild();
}

// Column sort (owner, run 30). Sorting reorders g.cats itself; ties fall back to
// the sql key so the order is stable between frames.
const ImGuiTableSortSpecs* g_sort;

int cmp_cats(const void* pa, const void* pb) {
    const lobby_cat& a = *(const lobby_cat*)pa;
    const lobby_cat& b = *(const lobby_cat*)pb;
    for (int k = 0; k < g_sort->SpecsCount; k++) {
        const ImGuiTableColumnSortSpecs& sp = g_sort->Specs[k];
        int d = 0;
        switch (sp.ColumnIndex) {
            case 0: d = strcmp(a.name, b.name); break;
            case 2: d = strcmp(class_name(a.cls), class_name(b.cls)); break;
            case 3: d = a.level - b.level; break;
            case 4: d = a.age_days - b.age_days; break;
            case 5: d = strcmp(room_name(a.room), room_name(b.room)); break;
            case 6: { int t = g.online ? g.my_team : 1; d = (b.team == t) - (a.team == t); break; }   // chosen first
            case 7: d = (b.team == 2) - (a.team == 2); break;
        }
        if (d) return sp.SortDirection == ImGuiSortDirection_Descending ? -d : d;
    }
    return a.key < b.key ? -1 : a.key > b.key;
}

void cat_table() {
    if (ImGui::BeginTable("cattab", g.online ? 7 : 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                         ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_SortTristate,
                          ImVec2(0, ImGui::GetIO().DisplaySize.y * 0.26f))) {
        ImGui::TableSetupColumn(TR("Кот", "Cat"), ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortAscending, 3.0f);
        ImGui::TableSetupColumn(TR("Навыки", "Skills"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 6 * 38.0f);
        ImGui::TableSetupColumn(TR("Класс", "Class"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn(TR("Ур.", "Lv."), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 52.0f);
        ImGui::TableSetupColumn(TR("Возраст", "Age"), ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn(TR("Комната", "Room"), ImGuiTableColumnFlags_WidthStretch, 1.8f);
        if (g.online) {   // owner, run 51: online, a row click picks the cat
            ImGui::TableSetupColumn(TR("В бой", "Fight"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
        } else {
            ImGui::TableSetupColumn(TR("И1", "P1"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn(TR("И2", "P2"), ImGuiTableColumnFlags_WidthFixed, 60.0f);
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs())
            if (ss->SpecsDirty) {
                if (ss->SpecsCount > 0 && g.ncats > 1) {
                    g_sort = ss;
                    qsort(g.cats, g.ncats, sizeof(lobby_cat), cmp_cats);
                }
                ss->SpecsDirty = false;
            }
        for (int i = 0; i < g.ncats; i++) {
            lobby_cat& c = g.cats[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (g.online) {
                bool on = c.team == g.my_team;
                bool full = !on && team_count(g.my_team) >= g.s.team_max;
                ImVec2 at = ImGui::GetCursorPos();
                if (full) ImGui::BeginDisabled();
                if (ImGui::Selectable("##pick", on, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0, 54))) {
                    c.team = on ? 0 : g.my_team;
                    c.pick = ++g.pick_seq;
                }
                if (full) ImGui::EndDisabled();
                ImGui::SetCursorPos(at);
            }
            cat_face(c, 52);
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 14);
            if (c.sex == 0)      ImGui::TextColored(ImVec4(0.20f, 0.40f, 0.80f, 1), "♂");
            else if (c.sex == 1) ImGui::TextColored(ImVec4(0.80f, 0.25f, 0.45f, 1), "♀");
            else                 ImGui::TextDisabled("·");
            ImGui::SameLine();
            ImGui::TextUnformatted(c.name);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", c.name);
                ImGui::Separator();
                static const char* kStat[7] = { TR("Сила", "Strength"), TR("Ловкость", "Dexterity"), TR("Телосложение", "Constitution"), TR("Интеллект", "Intelligence"), TR("Скорость", "Speed"), TR("Харизма", "Charisma"), TR("Удача", "Luck") };
                for (int k = 0; k < 7; k++) ImGui::Text("%-14s %d", kStat[k], c.stats[k]);
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
            skill_icons(c, 34);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(class_name(c.cls));
            ImGui::TableNextColumn(); ImGui::Text("%d", c.level);
            ImGui::TableNextColumn(); ImGui::Text(TR("%d дн.", "%d d"), c.age_days);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(room_name(c.room));
            if (g.online) {
                ImGui::TableNextColumn();
                if (c.team == g.my_team) ImGui::TextColored(ImVec4(0.55f, 0.20f, 0.20f, 1), TR("  да", "  yes"));
                else                     ImGui::TextDisabled("  —");
                ImGui::PopID();
                continue;
            }
            for (int t = 1; t <= 2; t++) {
                ImGui::TableNextColumn();
                bool on = c.team == t;
                if (g.online && t != g.my_team) { ImGui::TextDisabled("  —"); continue; }
                bool full = !on && team_count(t) >= g.s.team_max;
                if (full) ImGui::BeginDisabled();
                if (ImGui::Selectable(t == 1 ? TR("  И1  ", "  P1  ") : TR("  И2  ", "  P2  "), on)) { c.team = on ? 0 : t; c.pick = ++g.pick_seq; }
                if (full) ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void settings_body();

void settings() {
    bool ro = g.online && !g.is_host;
    if (ro) {
        ImGui::TextDisabled(TR("Правила задаёт хост.", "The host sets the rules."));
        ImGui::BeginDisabled();
    }
    settings_body();
    if (ro) ImGui::EndDisabled();
}

void settings_body() {
    // UI order differs from the probe's numbering (0 none, 1 random, 2 house, 3 balanced).
    static const char* kNames[] = { TR("Без вещей", "No items"), TR("Рандом", "Random"), TR("Сбалансированный рандом", "Balanced random"), TR("Из дома", "From home") };
    static const int kUiToPreset[] = { 0, 1, 3, 2 };
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt(TR("котов на сторону", "cats per side"), &g.s.team_max, 1, 4)) {
        for (int t = 1; t <= 2; t++) {   // drop the latest picks
            int idx[LOBBY_MAX_CATS], n = team_order(t, idx);
            for (int k = g.s.team_max; k < n; k++) g.cats[idx[k]].team = 0;
        }
    }
    int ui = 0;
    for (int i = 0; i < 4; i++) if (kUiToPreset[i] == g.s.item_preset) ui = i;
    ImGui::SetNextItemWidth(320);
    if (ImGui::Combo(TR("Вещи", "Items"), &ui, kNames, 4)) g.s.item_preset = kUiToPreset[ui];
    if (g.s.item_preset != 0) {
        ImGui::SameLine(0, 30);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt(g.s.item_preset == 2 ? TR("макс. вещей на кота", "max items per cat") : TR("вещей на кота", "items per cat"), &g.s.items_per_cat, 1, 5);
        if (g.s.item_preset == 2)
            ImGui::TextDisabled(g.online ? TR("Каждый игрок одевает своих котов из своего хранилища (%d у вас).", "Each player dresses their own cats from their own storage (%d in yours).")
                                         : TR("Игроки выберут вещи из хранилища на следующем шаге (%d в хранилище).", "Players pick items from the storage on the next step (%d in storage)."), g.nitems);
    }
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt(TR("укрытий", "cover"), &g.s.cover_count, 0, 16)) g.s.cover_count &= ~1;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(TR("Камни и прочие препятствия, расставляются зеркально для обеих сторон.\nНовая расстановка в каждом бою.", "Rocks and other obstacles, placed mirrored for both sides.\nA new layout every fight."));
    if (g.s.cover_count > 0) {
        static const char* kKinds[] = { TR("вперемешку", "mixed"), TR("разрушаемые", "breakable"), TR("неразрушаемые", "solid") };
        ImGui::SameLine(0, 30);
        ImGui::SetNextItemWidth(220);
        ImGui::Combo("##coverkind", &g.s.cover_kind, kKinds, 3);
    }
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(TR("мобов", "monsters"), &g.s.mob_count, 0, 8);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(TR("Монстры под управлением ИИ, парами зеркально (нечётный -- в случайном месте).\nВраги обоим игрокам; победу решают только коты.", "AI monsters, in mirrored pairs (an odd one in a random place).\nEnemies to both players; only the cats decide the winner."));
    if (g.s.mob_count > 0) {
        static const char* kKinds[] = { TR("слабые", "weak"), TR("средние", "medium"), TR("вперемешку", "mixed") };
        static const char* kModes[] = { TR("против обоих игроков", "against both players"), TR("каждый сам за себя", "every one for itself") };
        ImGui::SameLine(0, 30);
        ImGui::SetNextItemWidth(170);
        ImGui::Combo("##mobkind", &g.s.mob_kind, kKinds, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        ImGui::Combo("##mobmode", &g.s.mob_mode, kModes, 2);
        ImGui::SameLine();
        bool mad = g.s.mob_mad != 0;
        if (ImGui::Checkbox(TR("безумие", "madness"), &mad)) g.s.mob_mad = mad;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(TR("Мобы с Безумием бьют кого угодно рядом, без предпочтений.", "Monsters with Madness hit anyone nearby, with no preference."));
    }
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(TR("птиц на старте", "birds at start"), &g.s.bird_count, 0, 6);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(TR("Кот, убивший птицу, сразу надевает случайную вещь.\nПтицы улетают через несколько ходов.", "A cat that kills a bird puts on a random item at once.\nBirds fly away after a few turns."));
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(TR("птицы каждые N ходов", "birds every N turns"), &g.s.bird_every, 0, 6, g.s.bird_every ? "%d" : TR("нет", "off"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(TR("В начале раунда прилетают новые птицы.\n0 = только на старте.", "New birds arrive at the start of a round.\n0 = only at the start."));
    if (g.s.bird_every > 0) {
        ImGui::SameLine(0, 30);
        ImGui::SetNextItemWidth(160);
        if (g.s.bird_wave < 1) g.s.bird_wave = 1;
        ImGui::SliderInt(TR("макс. живых", "max alive"), &g.s.bird_wave, 1, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(TR("Волна досыпает птиц только до этого числа живых.", "A wave only tops birds up to this many alive."));
    }
    if (g.s.bird_count > 0 || g.s.bird_every > 0) {
        static const char* kBirds[3] = { TR("маленькие", "small"), TR("средние", "medium"), TR("большие", "large") };
        ImGui::TextDisabled(TR("птицы:", "birds:"));
        for (int k = 0; k < 3; k++) {
            ImGui::SameLine(0, k ? 14 : 20);
            bool on = (g.s.bird_mask >> k) & 1;
            if (ImGui::Checkbox(kBirds[k], &on)) g.s.bird_mask = on ? g.s.bird_mask | (1 << k) : g.s.bird_mask & ~(1 << k);
        }
        if (!(g.s.bird_mask & 7)) g.s.bird_mask = 1;   // at least one kind
    }
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt(TR("лута на земле", "loot on the ground"), &g.s.loot_count, 0, 20);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(TR("Монетки, мята и еда в случайных местах поля.", "Coins, catnip and food in random places on the field."));
    if (g.s.loot_count > 0) {
        static const char* kLoot[4] = { TR("монеты", "coins"), TR("мята", "catnip"), TR("еда", "food"), TR("скрап", "scrap") };
        for (int k = 0; k < 4; k++) {
            ImGui::SameLine(0, k ? 14 : 30);
            bool on = (g.s.loot_mask >> k) & 1;
            if (ImGui::Checkbox(kLoot[k], &on)) g.s.loot_mask = on ? g.s.loot_mask | (1 << k) : g.s.loot_mask & ~(1 << k);
        }
    }
    bool mon = g.s.money_on != 0;
    if (ImGui::Checkbox(TR("Стартовые деньги", "Starting money"), &mon)) g.s.money_on = mon;
    if (g.s.money_on) {
        ImGui::SameLine(0, 20);
        ImGui::SetNextItemWidth(160);
        ImGui::InputInt("##money", &g.s.money, 5, 20);
        if (g.s.money < 0) g.s.money = 0;
        if (g.s.money > 9999) g.s.money = 9999;
    }
}

void draw_pick(int t);

void draw() {
    ImGuiIO& io = ImGui::GetIO();
    // Owner, run 49: tall enough that СТАРТ / Отмена always fit.
    // Owner, online test: on a smaller screen the bottom did not fit. The window
    // now spans the screen from top to bottom (8 px margins), the lobby body
    // scrolls, and the buttons sit in a footer that is always visible.
    const float margin = 8.0f;
    float w = io.DisplaySize.x * 0.62f;
    if (w < 1000.0f) w = io.DisplaySize.x - 2 * margin < 1000.0f ? io.DisplaySize.x - 2 * margin : 1000.0f;
    ImVec2 size(w, io.DisplaySize.y - 2 * margin);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, margin), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::Begin(TR("PvP АРЕНА", "PvP ARENA"), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (g.page) { draw_pick(g.page); ImGui::End(); return; }
    const float footer = 48.0f + ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetStyle().WindowPadding.y + 6.0f;
    ImGui::BeginChild("body", ImVec2(0, -footer));
    if (ImGui::BeginTable("players", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); player_card(1);
        ImGui::TableNextColumn(); player_card(2);
        ImGui::EndTable();
    }
    ImGui::Separator();
    cat_table();
    ImGui::Separator();
    settings();
    ImGui::EndChild();
    ImGui::Separator();
    bool ok = team_count(1) > 0 && team_count(2) > 0;
    // Online "Из дома": each player dresses only their own cats, from their own
    // storage, whenever they like before the host starts; the kits ride in PICK.
    if (g.online && g.s.item_preset == 2) {
        bool mine = team_count(g.my_team) > 0;
        if (!mine) ImGui::BeginDisabled();
        if (ImGui::Button(TR("ВЕЩИ ИЗ ДОМА", "ITEMS FROM HOME"), ImVec2(220, 48))) { g.page = g.my_team; g.sel_cat = -1; }
        if (!mine) ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (g.online && !g.is_host) {   // only the host starts an online fight
        if (ImGui::Button(TR("Отмена", "Cancel"), ImVec2(160, 48))) { g.open = false; g.cancelled = true; }
        ImGui::SameLine();
        ImGui::TextDisabled(ok ? TR("Ждём, пока хост нажмёт СТАРТ.", "Waiting for the host to press START.") : TR("Выберите котов; хост начнёт бой.", "Pick your cats; the host starts the fight."));
        ImGui::End();
        return;
    }
    if (!ok) ImGui::BeginDisabled();
    bool house = g.s.item_preset == 2 && !g.online;
    if (ImGui::Button(house ? TR("ДАЛЕЕ: ВЕЩИ", "NEXT: ITEMS") : TR("СТАРТ", "START"), ImVec2(220, 48))) {
        if (house) { g.page = 1; g.sel_cat = -1; }
        else { g.start_pending = true; g.open = false; }
    }
    if (!ok) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(TR("Отмена", "Cancel"), ImVec2(160, 48))) { g.open = false; g.cancelled = true; }
    if (!ok) { ImGui::SameLine(); ImGui::TextDisabled(TR("нужен хотя бы один кот у каждого игрока", "each player needs at least one cat")); }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// House preset, step two: each player in turn dresses their cats from the storage.
// An item can be taken once across both players (hotseat shares one house).

const item_text* item_info(const char* id) {
    int n = 0;
    const item_text* tab = gd_items(g_lobby_en, &n);
    if (!tab) return nullptr;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(id, tab[mid].id);
        if (!c) return &tab[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return nullptr;
}

const char* item_name(const char* id) {
    const item_text* it = item_info(id);
    return it ? it->name : id;
}

ImVec4 rarity_colour(const char* id) {
    static const ImVec4 k[5] = {
        ImVec4(0.30f, 0.29f, 0.27f, 1), ImVec4(0.18f, 0.50f, 0.20f, 1), ImVec4(0.18f, 0.38f, 0.78f, 1),
        ImVec4(0.55f, 0.22f, 0.68f, 1), ImVec4(0.65f, 0.25f, 0.15f, 1) };
    const item_text* it = item_info(id);
    return k[it && it->rarity >= 0 && it->rarity < 5 ? it->rarity : 4];
}

void item_tooltip(const char* id) {
    const item_text* it = item_info(id);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
    ImGui::TextColored(rarity_colour(id), "%s", it ? it->name : id);
    if (it && it->stats[0]) ImGui::TextUnformatted(it->stats);
    if (it && it->desc[0]) { ImGui::Separator(); ImGui::TextUnformatted(it->desc); }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// A round plate like the game's storage slots, with the item's icon on it.
void draw_icon(ImDrawList* dl, const char* id, ImVec2 p, float sz) {
    ImVec2 c(p.x + sz / 2, p.y + sz / 2);
    dl->AddCircleFilled(c, sz / 2, IM_COL32(205, 202, 194, 255));
    if (!id || !id[0]) return;
    int w = 0, h = 0;
    unsigned tex = portraits_item(gd_item_kind(id), gd_item_frame(id), 96, &w, &h);
    if (!tex || w <= 0 || h <= 0) return;
    float in = sz * 0.10f, box = sz - 2 * in;
    float k = box / (float)(w > h ? w : h), dw = w * k, dh = h * k;
    ImVec2 a(p.x + (sz - dw) / 2, p.y + (sz - dh) / 2);
    dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)tex), a, ImVec2(a.x + dw, a.y + dh));
}

// One selectable row: icon, name in its rarity colour, then the flat stats.
bool item_row(const char* id, const char* label, bool sel, float h) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_icon(dl, id, p, h);
    float ty = p.y + (h - ImGui::GetTextLineHeight()) / 2;
    float alpha = ImGui::GetStyle().Alpha * (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled ? 0.5f : 1.0f);
    ImVec4 col = id && id[0] ? rarity_colour(id) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    col.w *= alpha;
    dl->AddText(ImVec2(p.x + h + 10, ty), ImGui::GetColorU32(col), label);
    const item_text* info = id && id[0] ? item_info(id) : nullptr;
    if (info && info->stats[0]) {
        float x = p.x + h + 10 + ImGui::CalcTextSize(label).x + 14;
        ImVec4 dc = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
        dc.w *= alpha;
        dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(dc), info->stats);
    }
    return clicked;
}

bool item_taken(long long id) {
    for (int i = 0; i < g.ncats; i++)
        if (g.cats[i].team)
            for (int s = 0; s < LOBBY_SLOTS; s++)
                if (g.cats[i].kit[s][0] && g.cats[i].kit_id[s] == id) return true;
    return false;
}

int kit_count(const lobby_cat& c) {
    int n = 0;
    for (int s = 0; s < LOBBY_SLOTS; s++) n += c.kit[s][0] != 0;
    return n;
}

void put_item(lobby_cat& c, int slot, const lobby_item* it) {
    if (it) { strcpy(c.kit[slot], it->name); c.kit_id[slot] = it->id; }
    else    { c.kit[slot][0] = 0; c.kit_id[slot] = 0; }
}

void random_kits(int t) {
    int idx[LOBBY_MAX_CATS], n = team_order(t, idx);
    // A fresh roll each press (owner, run 33): this player's kits go back first.
    for (int k = 0; k < n; k++)
        for (int s = 0; s < LOBBY_SLOTS; s++) put_item(g.cats[idx[k]], s, nullptr);
    for (int k = 0; k < n; k++) {
        lobby_cat& c = g.cats[idx[k]];
        int order[LOBBY_SLOTS] = { 0, 1, 2, 3, 4 };
        for (int i = LOBBY_SLOTS - 1; i > 0; i--) { int j = rand() % (i + 1); int x = order[i]; order[i] = order[j]; order[j] = x; }
        for (int i = 0; i < LOBBY_SLOTS && kit_count(c) < g.s.items_per_cat; i++) {
            int s = order[i];
            if (c.kit[s][0]) continue;
            static int free_items[LOBBY_MAX_ITEMS];
            int nf = 0;
            for (int j = 0; j < g.nitems; j++)
                if (g.items[j].slot == s && !item_taken(g.items[j].id)) free_items[nf++] = j;
            if (nf) put_item(c, s, &g.items[free_items[rand() % nf]]);
        }
    }
}

void draw_pick(int t) {
    static const char* kSlot[LOBBY_SLOTS] = { TR("Голова", "Head"), TR("Лицо", "Face"), TR("Шея", "Neck"), TR("Оружие", "Weapon"), TR("Брелок", "Trinket") };
    int idx[LOBBY_MAX_CATS], n = team_order(t, idx);
    if (g.sel_cat < 0 || g.cats[g.sel_cat].team != t) { g.sel_cat = n ? idx[0] : -1; g.sel_slot = 3; }
    ImGui::Text(TR("Вещи из дома: %s", "Items from home: %s"), g.s.player_name[t - 1]);
    ImGui::SameLine();
    ImGui::TextDisabled(g.online ? TR("(до %d вещей на кота, из вашего хранилища)", "(up to %d items per cat, from your storage)")
                                 : TR("(до %d вещей на кота; взятую вещь второй игрок уже не возьмёт)", "(up to %d items per cat; an item taken cannot be taken by the other player)"), g.s.items_per_cat);
    ImGui::Separator();
    float h = ImGui::GetContentRegionAvail().y - 70;
    // Left: the player's cats, five slots each.
    ImGui::BeginChild("kits", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, h), ImGuiChildFlags_Borders);
    for (int k = 0; k < n; k++) {
        lobby_cat& c = g.cats[idx[k]];
        ImGui::PushID(idx[k]);
        cat_face(c, 60);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 18);
        ImGui::Text("%s", c.name);
        ImGui::SameLine();
        ImGui::TextDisabled(TR("%s, ур. %d  —  %d / %d", "%s, lv. %d  —  %d / %d"), class_name(c.cls), c.level, kit_count(c), g.s.items_per_cat);
        if (ImGui::BeginTable("slots", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("s", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("i", ImGuiTableColumnFlags_WidthStretch);
            for (int s = 0; s < LOBBY_SLOTS; s++) {
                ImGui::PushID(s);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
                ImGui::TextDisabled("%s", kSlot[s]);
                ImGui::TableNextColumn();
                bool sel = g.sel_cat == idx[k] && g.sel_slot == s;
                bool has = c.kit[s][0] != 0;
                if (item_row(c.kit[s], has ? item_name(c.kit[s]) : "—", sel, 40)) { g.sel_cat = idx[k]; g.sel_slot = s; }
                if (has) {
                    if (ImGui::IsItemHovered()) item_tooltip(c.kit[s]);
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) put_item(c, s, nullptr);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::TextDisabled(TR("ЛКМ по слоту — выбрать, ПКМ по вещи — снять", "LMB on a slot to select, RMB on an item to remove"));
    ImGui::EndChild();
    ImGui::SameLine();
    // Right: free storage items for the selected slot.
    ImGui::BeginChild("store", ImVec2(0, h), ImGuiChildFlags_Borders);
    if (g.sel_cat >= 0) {
        lobby_cat& c = g.cats[g.sel_cat];
        ImGui::Text("%s — %s", c.name, kSlot[g.sel_slot]);
        bool full = !c.kit[g.sel_slot][0] && kit_count(c) >= g.s.items_per_cat;
        if (full) ImGui::TextColored(ImVec4(0.65f, 0.25f, 0.15f, 1), TR("У кота уже %d вещей — сними одну.", "This cat already has %d items -- remove one."), g.s.items_per_cat);
        ImGui::Separator();
        int shown = 0;
        if (full) ImGui::BeginDisabled();
        for (int j = 0; j < g.nitems; j++) {
            const lobby_item& it = g.items[j];
            if (it.slot != g.sel_slot || item_taken(it.id)) continue;
            shown++;
            ImGui::PushID(j);
            if (item_row(it.name, item_name(it.name), false, 48)) put_item(c, g.sel_slot, &it);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) item_tooltip(it.name);
            ImGui::PopID();
        }
        if (full) ImGui::EndDisabled();
        if (!shown) ImGui::TextDisabled(TR("В хранилище нет свободных вещей для этого слота.", "No free items in the storage for this slot."));
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (g.online) {   // one player, own storage: back to the lobby when done
        if (ImGui::Button(TR("Случайно", "Random"), ImVec2(160, 48))) random_kits(t);
        ImGui::SameLine();
        if (ImGui::Button(TR("Очистить", "Clear"), ImVec2(160, 48)))
            for (int k = 0; k < n; k++) for (int s = 0; s < LOBBY_SLOTS; s++) put_item(g.cats[idx[k]], s, nullptr);
        ImGui::SameLine(0, 40);
        if (ImGui::Button(TR("ГОТОВО", "DONE"), ImVec2(220, 48))) { g.page = 0; g.sel_cat = -1; }
        return;
    }
    if (ImGui::Button(TR("← Назад", "← Back"), ImVec2(160, 48))) { g.page = t == 1 ? 0 : 1; g.sel_cat = -1; }
    ImGui::SameLine();
    if (ImGui::Button(TR("Случайно", "Random"), ImVec2(160, 48))) random_kits(t);
    ImGui::SameLine();
    if (ImGui::Button(TR("Очистить", "Clear"), ImVec2(160, 48)))
        for (int k = 0; k < n; k++) for (int s = 0; s < LOBBY_SLOTS; s++) put_item(g.cats[idx[k]], s, nullptr);
    ImGui::SameLine(0, 40);
    if (t == 1) {
        if (ImGui::Button(TR("ДАЛЕЕ: ИГРОК 2", "NEXT: PLAYER 2"), ImVec2(240, 48))) { g.page = 2; g.sel_cat = -1; }
    } else if (ImGui::Button(TR("В БОЙ", "FIGHT"), ImVec2(220, 48))) {
        g.page = 0; g.start_pending = true; g.open = false;
    }
}

int cmp_items(const void* pa, const void* pb) {
    // By slot, then rarity (rarest first), then shown name.
    const lobby_item& a = *(const lobby_item*)pa;
    const lobby_item& b = *(const lobby_item*)pb;
    if (a.slot != b.slot) return a.slot - b.slot;
    const item_text* ia = item_info(a.name);
    const item_text* ib = item_info(b.name);
    int ra = ia ? ia->rarity : 4, rb = ib ? ib->rarity : 4;
    if (ra != rb) return rb - ra;
    return strcmp(item_name(a.name), item_name(b.name));
}

} // namespace

extern "C" void lobby_set_storage(const lobby_item* items, int n) {
    g.nitems = n < LOBBY_MAX_ITEMS ? n : LOBBY_MAX_ITEMS;
    memcpy(g.items, items, sizeof(lobby_item) * g.nitems);
    qsort(g.items, g.nitems, sizeof(lobby_item), cmp_items);
}

extern "C" void lobby_set_log(lobby_log_fn f) { g.log = f; }
extern "C" void lobby_set_english(int en) { g_lobby_en = en ? 1 : 0; }

extern "C" void lobby_open(const lobby_cat* cats, int n, const lobby_settings* s) {
    g.ncats = n < LOBBY_MAX_CATS ? n : LOBBY_MAX_CATS;
    memcpy(g.cats, cats, sizeof(lobby_cat) * g.ncats);
    g.s = *s;
    for (int i = 0; i < g.ncats; i++) g.cats[i].pick = i;
    g.pick_seq = g.ncats;
    if (g.s.team_max < 1) g.s.team_max = 1;
    if (g.s.team_max > 4) g.s.team_max = 4;
    if (g.s.items_per_cat < 1) g.s.items_per_cat = 1;
    g.start_pending = false;
    g.page = 0;
    g.sel_cat = -1;
    g.open = true;
    g.online = false;
    g.cancelled = false;
    if (g.ready) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g.ctx);
        ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse();
        ImGui::SetCurrentContext(prev);
    }
    LOG("lobby: opened with %d cat(s)", g.ncats);
}

extern "C" int lobby_is_open(void) { return g.open; }

extern "C" void lobby_open_online(const lobby_cat* cats, int n, const lobby_settings* s, int is_host) {
    lobby_open(cats, n, s);
    for (int i = 0; i < g.ncats; i++) g.cats[i].team = 0;   // nothing preassigned online
    g.online = true;
    g.is_host = is_host != 0;
    g.my_team = is_host ? 1 : 2;
    g.nremote = 0;
    g.remote_name[0] = 0;
    LOG("lobby: online, this player is team %d (%s)", g.my_team, is_host ? "host" : "client");
}

extern "C" void lobby_set_remote(const char* name, const lobby_cat* cats, int n) {
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    strncpy_s(g.remote_name, name ? name : "", _TRUNCATE);
    memcpy(g.remote, cats, sizeof(lobby_cat) * n);
    for (int i = 0; i < n; i++) g.remote[i].team = 3 - g.my_team;
    g.nremote = n;
}

extern "C" void lobby_set_settings(const lobby_settings* s) {
    char mine[32];
    int t = g.my_team ? g.my_team : 2;
    memcpy(mine, g.s.player_name[t - 1], sizeof mine);
    g.s = *s;
    memcpy(g.s.player_name[t - 1], mine, sizeof mine);
    if (g.s.team_max < 1) g.s.team_max = 1;
    if (g.s.team_max > 4) g.s.team_max = 4;
    int idx[LOBBY_MAX_CATS], n = team_order(t, idx);   // the host may have lowered the cap
    for (int k = g.s.team_max; k < n; k++) g.cats[idx[k]].team = 0;
}

extern "C" int lobby_get_local(lobby_cat* cats, char* name32, lobby_settings* s) {
    int t = g.my_team ? g.my_team : 1;
    int idx[LOBBY_MAX_CATS], n = team_order(t, idx);
    for (int k = 0; k < n; k++) cats[k] = g.cats[idx[k]];
    if (name32) memcpy(name32, g.s.player_name[t - 1], 32);
    if (s) *s = g.s;
    return n;
}

extern "C" void lobby_close(void) {
    g.open = false;
    g.start_pending = false;
    g.online = false;
}

extern "C" int lobby_take_cancel(void) {
    bool c = g.cancelled;
    g.cancelled = false;
    if (c) g.online = false;
    return c ? 1 : 0;
}

extern "C" int lobby_is_online(void) { return g.online ? 1 : 0; }

extern "C" int lobby_auto_pick(unsigned seed) {
    if (!g.open || !g.online || !g.my_team) return 0;
    const int t = g.my_team;
    for (int i = 0; i < g.ncats; i++) if (g.cats[i].team == t) g.cats[i].team = 0;
    int idx[LOBBY_MAX_CATS];
    for (int i = 0; i < g.ncats; i++) idx[i] = i;
    for (int i = g.ncats - 1; i > 0; i--) {   // Fisher-Yates on a small LCG
        seed = seed * 1103515245u + 12345u;
        int j = (int)((seed >> 16) % (unsigned)(i + 1));
        int x = idx[i]; idx[i] = idx[j]; idx[j] = x;
    }
    int got = 0;
    for (int k = 0; k < g.ncats && got < g.s.team_max; k++) {
        g.cats[idx[k]].team = t;
        g.cats[idx[k]].pick = ++g.pick_seq;
        got++;
    }
    LOG("lobby: auto-picked %d cat(s) for team %d", got, t);
    return got;
}

extern "C" void lobby_auto_kits(void) {
    if (!g.open || !g.online || !g.my_team) return;
    random_kits(g.my_team);
    LOG("lobby: auto-dressed team %d from the storage", g.my_team);
}

extern "C" int lobby_local_count(void) { return g.open && g.online ? team_count(g.my_team) : 0; }

extern "C" void lobby_on_swap(void) {
    if (!g.open || g.failed) return;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    if (!g.ready) {
        if (!bring_up()) { g.failed = true; g.open = false; ImGui::SetCurrentContext(prev); return; }
        g.ready = true;
    }
    ImGui::SetCurrentContext(g.ctx);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    ImGui::SetCurrentContext(prev);
}

extern "C" int lobby_take_start(lobby_cat* cats, int* n, lobby_settings* s) {
    if (!g.start_pending) return 0;
    g.start_pending = false;
    int k = 0;
    for (int t = 1; t <= 2; t++) {   // by team, in pick order
        int idx[LOBBY_MAX_CATS], n = team_order(t, idx);
        for (int j = 0; j < n; j++) cats[k++] = g.cats[idx[j]];
    }
    *n = k;
    *s = g.s;
    return 1;
}
