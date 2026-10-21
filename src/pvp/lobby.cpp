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
#include "items_text_gen.h"
#include "items_icon_gen.h"
#include <GL/gl.h>
// stb_image's implementation is in mgmp_overlay.cpp (built without stdio).
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "portraits.h"
#include "skills_text_gen.h"

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
    lobby_log_fn log = nullptr;

    lobby_cat      cats[LOBBY_MAX_CATS];
    int            ncats = 0;
    lobby_settings s{};
    int            pick_seq = 0;

    lobby_item     items[LOBBY_MAX_ITEMS];
    int            nitems = 0;
    int            page = 0;          // 0 the lobby, 1 / 2 that player's item picking
    int            sel_cat = -1, sel_slot = 0;
    GLuint         icons = 0;         // item icon atlas (pvp_probe_icons.png next to the dll)
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
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { g.open = false; return 0; }
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

// The item icon atlas, made by pvp/tools/make_item_icons.py from the game's art.
// Optional: without it the picker shows names only.
void load_icons() {
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&load_icons, &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "pvp_probe_icons.png");
    int w = 0, h = 0, comp = 0;
    unsigned char* px = nullptr;
    if (FILE* f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char* file = (unsigned char*)malloc(n > 0 ? n : 1);
        if (file && fread(file, 1, n, f) == (size_t)n) px = stbi_load_from_memory(file, (int)n, &w, &h, &comp, 4);
        free(file);
        fclose(f);
    }
    if (!px) { LOG("lobby: no icon atlas at %s", path); return; }
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glGenTextures(1, &g.icons);
    glBindTexture(GL_TEXTURE_2D, g.icons);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    stbi_image_free(px);
    LOG("lobby: icon atlas %dx%d loaded", w, h);
}

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
    load_icons();
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
    for (auto& c : kClassText) if (!_stricmp(cls, c.id_upper)) return c.name;
    return cls;
}

// House rooms (data/house.gon). The game never names them on screen, so these
// are ours.
const char* room_name(const char* room) {
    static const char* kMap[][2] = {
        { "Floor1_Large", "1 этаж, большая" }, { "Floor1_Small", "1 этаж, малая" },
        { "Floor2_Large", "2 этаж, большая" }, { "Floor2_Small", "2 этаж, малая" },
        { "Attic", "Чердак" }, { "SmallAttic", "Чердак" }, { "LargeAttic", "Чердак" },
        { "Basement0", "Подвал" }, { "Basement1", "Пещеры" }, { "Basement2", "Глубины" },
        { "Basement3", "Утроба" }, { "Basement4", "Шеол" },
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
        const skill_text* info = passive
            ? skill_info(kPassiveText, (int)(sizeof kPassiveText / sizeof kPassiveText[0]), id)
            : skill_info(kAbilityText, (int)(sizeof kAbilityText / sizeof kAbilityText[0]), id);
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
        else dl->AddText(ImVec2(p.x + 4, p.y + 4), IM_COL32(90, 86, 80, 255), passive ? "П" : "С");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
            ImGui::TextColored(passive ? ImVec4(0.18f, 0.38f, 0.78f, 1) : ImVec4(0.55f, 0.20f, 0.20f, 1), "%s",
                               info ? info->name : id);
            ImGui::TextDisabled(passive ? "пассивка" : "способность");
            if (info && info->desc[0]) { ImGui::Separator(); ImGui::TextUnformatted(info->desc); }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    if (!any) ImGui::TextDisabled("—");
}

int team_count(int t) {
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
    ImGui::Text("Игрок %d", t);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText(t == 1 ? "##n1" : "##n2", g.s.player_name[t - 1], sizeof g.s.player_name[0]);
    ImGui::Text("Коты: %d / %d", team_count(t), g.s.team_max);
    ImGui::EndGroup();
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
            case 6: d = (b.team == 1) - (a.team == 1); break;   // chosen first
            case 7: d = (b.team == 2) - (a.team == 2); break;
        }
        if (d) return sp.SortDirection == ImGuiSortDirection_Descending ? -d : d;
    }
    return a.key < b.key ? -1 : a.key > b.key;
}

void cat_table() {
    if (ImGui::BeginTable("cattab", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                         ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY |
                                         ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti | ImGuiTableFlags_SortTristate,
                          ImVec2(0, ImGui::GetIO().DisplaySize.y * 0.26f))) {
        ImGui::TableSetupColumn("Кот", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortAscending, 3.0f);
        ImGui::TableSetupColumn("Навыки", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 6 * 38.0f);
        ImGui::TableSetupColumn("Класс", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("Ур.", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, 52.0f);
        ImGui::TableSetupColumn("Возраст", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Комната", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("И1", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("И2", ImGuiTableColumnFlags_WidthFixed, 60.0f);
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
                static const char* kStat[7] = { "Сила", "Ловкость", "Телосложение", "Интеллект", "Скорость", "Харизма", "Удача" };
                for (int k = 0; k < 7; k++) ImGui::Text("%-14s %d", kStat[k], c.stats[k]);
                ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
            skill_icons(c, 34);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(class_name(c.cls));
            ImGui::TableNextColumn(); ImGui::Text("%d", c.level);
            ImGui::TableNextColumn(); ImGui::Text("%d дн.", c.age_days);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(room_name(c.room));
            for (int t = 1; t <= 2; t++) {
                ImGui::TableNextColumn();
                bool on = c.team == t;
                bool full = !on && team_count(t) >= g.s.team_max;
                if (full) ImGui::BeginDisabled();
                if (ImGui::Selectable(t == 1 ? "  И1  " : "  И2  ", on)) { c.team = on ? 0 : t; c.pick = ++g.pick_seq; }
                if (full) ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void settings() {
    // UI order differs from the probe's numbering (0 none, 1 random, 2 house, 3 balanced).
    static const char* kNames[] = { "Без вещей", "Рандом", "Сбалансированный рандом", "Из дома" };
    static const int kUiToPreset[] = { 0, 1, 3, 2 };
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("котов на сторону", &g.s.team_max, 1, 4)) {
        for (int t = 1; t <= 2; t++) {   // drop the latest picks
            int idx[LOBBY_MAX_CATS], n = team_order(t, idx);
            for (int k = g.s.team_max; k < n; k++) g.cats[idx[k]].team = 0;
        }
    }
    int ui = 0;
    for (int i = 0; i < 4; i++) if (kUiToPreset[i] == g.s.item_preset) ui = i;
    ImGui::SetNextItemWidth(320);
    if (ImGui::Combo("Вещи", &ui, kNames, 4)) g.s.item_preset = kUiToPreset[ui];
    if (g.s.item_preset != 0) {
        ImGui::SameLine(0, 30);
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt(g.s.item_preset == 2 ? "макс. вещей на кота" : "вещей на кота", &g.s.items_per_cat, 1, 5);
        if (g.s.item_preset == 2)
            ImGui::TextDisabled("Игроки выберут вещи из хранилища на следующем шаге (%d в хранилище).", g.nitems);
    }
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("укрытий", &g.s.cover_count, 0, 16)) g.s.cover_count &= ~1;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Камни и прочие препятствия, расставляются зеркально для обеих сторон.\nНовая расстановка в каждом бою.");
    if (g.s.cover_count > 0) {
        static const char* kKinds[] = { "вперемешку", "разрушаемые", "неразрушаемые" };
        ImGui::SameLine(0, 30);
        ImGui::SetNextItemWidth(220);
        ImGui::Combo("##coverkind", &g.s.cover_kind, kKinds, 3);
    }
    ImGui::SetNextItemWidth(160);
    if (ImGui::SliderInt("мобов", &g.s.mob_count, 0, 8)) g.s.mob_count &= ~1;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Монстры под управлением ИИ, парами зеркально.\nВраги обоим игрокам; победу решают только коты.");
    if (g.s.mob_count > 0) {
        static const char* kKinds[] = { "слабые", "средние", "вперемешку" };
        static const char* kModes[] = { "против обоих игроков", "каждый сам за себя" };
        ImGui::SameLine(0, 30);
        ImGui::SetNextItemWidth(170);
        ImGui::Combo("##mobkind", &g.s.mob_kind, kKinds, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        ImGui::Combo("##mobmode", &g.s.mob_mode, kModes, 2);
        ImGui::SameLine();
        bool mad = g.s.mob_mad != 0;
        if (ImGui::Checkbox("безумие", &mad)) g.s.mob_mad = mad;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Мобы с Безумием бьют кого угодно рядом, без предпочтений.");
    }
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("птиц на старте", &g.s.bird_count, 0, 6);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Кот, убивший птицу, сразу надевает случайную вещь.\nПтицы улетают через несколько ходов.");
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("птицы каждые N ходов", &g.s.bird_every, 0, 6, g.s.bird_every ? "%d" : "нет");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("В начале раунда прилетают новые птицы.\n0 = только на старте.");
    if (g.s.bird_every > 0) {
        ImGui::SameLine(0, 30);
        ImGui::SetNextItemWidth(160);
        if (g.s.bird_wave < 1) g.s.bird_wave = 1;
        ImGui::SliderInt("макс. живых", &g.s.bird_wave, 1, 3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Волна досыпает птиц только до этого числа живых.");
    }
    if (g.s.bird_count > 0 || g.s.bird_every > 0) {
        static const char* kBirds[3] = { "маленькие", "средние", "большие" };
        ImGui::TextDisabled("птицы:");
        for (int k = 0; k < 3; k++) {
            ImGui::SameLine(0, k ? 14 : 20);
            bool on = (g.s.bird_mask >> k) & 1;
            if (ImGui::Checkbox(kBirds[k], &on)) g.s.bird_mask = on ? g.s.bird_mask | (1 << k) : g.s.bird_mask & ~(1 << k);
        }
        if (!(g.s.bird_mask & 7)) g.s.bird_mask = 1;   // at least one kind
    }
    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("лута на земле", &g.s.loot_count, 0, 20);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Монетки, мята и еда в случайных местах поля.");
    if (g.s.loot_count > 0) {
        static const char* kLoot[4] = { "монеты", "мята", "еда", "скрап" };
        for (int k = 0; k < 4; k++) {
            ImGui::SameLine(0, k ? 14 : 30);
            bool on = (g.s.loot_mask >> k) & 1;
            if (ImGui::Checkbox(kLoot[k], &on)) g.s.loot_mask = on ? g.s.loot_mask | (1 << k) : g.s.loot_mask & ~(1 << k);
        }
    }
    bool mon = g.s.money_on != 0;
    if (ImGui::Checkbox("Стартовые деньги", &mon)) g.s.money_on = mon;
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
    ImVec2 size(io.DisplaySize.x * 0.62f, io.DisplaySize.y * 0.95f);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("PvP АРЕНА", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    if (g.page) { draw_pick(g.page); ImGui::End(); return; }
    if (ImGui::BeginTable("players", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn(); player_card(1);
        ImGui::TableNextColumn(); player_card(2);
        ImGui::EndTable();
    }
    ImGui::Separator();
    cat_table();
    ImGui::Separator();
    settings();
    ImGui::Separator();
    bool ok = team_count(1) > 0 && team_count(2) > 0;
    if (!ok) ImGui::BeginDisabled();
    bool house = g.s.item_preset == 2;
    if (ImGui::Button(house ? "ДАЛЕЕ: ВЕЩИ" : "СТАРТ", ImVec2(220, 48))) {
        if (house) { g.page = 1; g.sel_cat = -1; }
        else { g.start_pending = true; g.open = false; }
    }
    if (!ok) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Отмена", ImVec2(160, 48))) g.open = false;
    if (!ok) { ImGui::SameLine(); ImGui::TextDisabled("нужен хотя бы один кот у каждого игрока"); }
    ImGui::End();
}

// ---------------------------------------------------------------------------
// House preset, step two: each player in turn dresses their cats from the storage.
// An item can be taken once across both players (hotseat shares one house).

const item_text* item_info(const char* id) {
    int lo = 0, hi = (int)(sizeof kItemText / sizeof kItemText[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(id, kItemText[mid].id);
        if (!c) return &kItemText[mid];
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

int icon_cell(const char* id) {
    int lo = 0, hi = (int)(sizeof kItemIcon / sizeof kItemIcon[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(id, kItemIcon[mid].id);
        if (!c) return kItemIcon[mid].cell;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

// A round plate like the game's storage slots, with the item's icon on it.
void draw_icon(ImDrawList* dl, const char* id, ImVec2 p, float sz) {
    ImVec2 c(p.x + sz / 2, p.y + sz / 2);
    dl->AddCircleFilled(c, sz / 2, IM_COL32(205, 202, 194, 255));
    int cell = id && id[0] ? icon_cell(id) : -1;
    if (!g.icons || cell < 0) return;
    float u = 1.0f / ITEM_ICON_COLS, v = 1.0f / ITEM_ICON_ROWS;
    float cu = (cell % ITEM_ICON_COLS) * u, cv = (cell / ITEM_ICON_COLS) * v;
    float in = sz * 0.08f;
    dl->AddImage(ImTextureRef((ImTextureID)(intptr_t)g.icons), ImVec2(p.x + in, p.y + in),
                 ImVec2(p.x + sz - in, p.y + sz - in), ImVec2(cu, cv), ImVec2(cu + u, cv + v));
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
    static const char* kSlot[LOBBY_SLOTS] = { "Голова", "Лицо", "Шея", "Оружие", "Брелок" };
    int idx[LOBBY_MAX_CATS], n = team_order(t, idx);
    if (g.sel_cat < 0 || g.cats[g.sel_cat].team != t) { g.sel_cat = n ? idx[0] : -1; g.sel_slot = 3; }
    ImGui::Text("Вещи из дома: %s", g.s.player_name[t - 1]);
    ImGui::SameLine();
    ImGui::TextDisabled("(до %d вещей на кота; взятую вещь второй игрок уже не возьмёт)", g.s.items_per_cat);
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
        ImGui::TextDisabled("%s, ур. %d  —  %d / %d", class_name(c.cls), c.level, kit_count(c), g.s.items_per_cat);
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
    ImGui::TextDisabled("ЛКМ по слоту — выбрать, ПКМ по вещи — снять");
    ImGui::EndChild();
    ImGui::SameLine();
    // Right: free storage items for the selected slot.
    ImGui::BeginChild("store", ImVec2(0, h), ImGuiChildFlags_Borders);
    if (g.sel_cat >= 0) {
        lobby_cat& c = g.cats[g.sel_cat];
        ImGui::Text("%s — %s", c.name, kSlot[g.sel_slot]);
        bool full = !c.kit[g.sel_slot][0] && kit_count(c) >= g.s.items_per_cat;
        if (full) ImGui::TextColored(ImVec4(0.65f, 0.25f, 0.15f, 1), "У кота уже %d вещей — сними одну.", g.s.items_per_cat);
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
        if (!shown) ImGui::TextDisabled("В хранилище нет свободных вещей для этого слота.");
    }
    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button("← Назад", ImVec2(160, 48))) { g.page = t == 1 ? 0 : 1; g.sel_cat = -1; }
    ImGui::SameLine();
    if (ImGui::Button("Случайно", ImVec2(160, 48))) random_kits(t);
    ImGui::SameLine();
    if (ImGui::Button("Очистить", ImVec2(160, 48)))
        for (int k = 0; k < n; k++) for (int s = 0; s < LOBBY_SLOTS; s++) put_item(g.cats[idx[k]], s, nullptr);
    ImGui::SameLine(0, 40);
    if (t == 1) {
        if (ImGui::Button("ДАЛЕЕ: ИГРОК 2", ImVec2(240, 48))) { g.page = 2; g.sel_cat = -1; }
    } else if (ImGui::Button("В БОЙ", ImVec2(220, 48))) {
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
    if (g.ready) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(g.ctx);
        ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse();
        ImGui::SetCurrentContext(prev);
    }
    LOG("lobby: opened with %d cat(s)", g.ncats);
}

extern "C" int lobby_is_open(void) { return g.open; }

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
