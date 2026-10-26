// gamedata.cpp -- see gamedata.h. A C++ port of pvp/tools/make_item_text.py,
// make_skill_text.py and make_item_table.py, run against the player's own game.

#include "gamedata.h"
#include "portrait/gpak.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct Item {
    std::string id, kind;
    std::string name[2], desc[2], stats[2];
    int rarity = 4, frame = -1;
};
struct Skill { std::string id, name[2], desc[2], base; };
struct Class { std::string id, name[2]; };

struct State {
    std::atomic<bool> started{false}, ready{false};
    gd_log_fn log = nullptr;
    std::vector<Item>  items;     // sorted by id
    std::vector<Class> classes;
    std::vector<Skill> abilities, passives;
    std::vector<std::string> slot_pool[5];
    std::unordered_map<std::string, size_t> item_index;
    // The C views handed out (pointers into the vectors above, built once).
    std::vector<item_text>  item_view[2];
    std::vector<class_text> class_view[2];
    std::vector<skill_text> abil_view[2], pass_view[2];
} g;

#define LOG(...) do { if (g.log) g.log(__VA_ARGS__); } while (0)

using Keys = std::map<std::string, std::string>;

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
bool is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }
std::string strip_quotes(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == '"') ++a;
    while (b > a && s[b - 1] == '"') --b;
    return s.substr(a, b - a);
}

// --- combined.csv -----------------------------------------------------------
// RFC 4180: quoted fields may hold commas, doubled quotes and newlines.
std::vector<std::vector<std::string>> parse_csv(const std::string& t) {
    std::vector<std::vector<std::string>> rows(1);
    std::string f;
    bool q = false;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < t.size() && t[i + 1] == '"') { f += '"'; ++i; }
                else q = false;
            } else f += c;
        } else if (c == '"') q = true;
        else if (c == ',') { rows.back().push_back(f); f.clear(); }
        else if (c == '\n') { rows.back().push_back(f); f.clear(); rows.emplace_back(); }
        else if (c != '\r') f += c;
    }
    if (!f.empty() || !rows.back().empty()) rows.back().push_back(f);
    return rows;
}

// --- text cleanup: [img:str] -> the stat's short name, other markup dropped ---
const char* const kStatKey[7] = { "str", "dex", "con", "int", "spd", "cha", "lck" };
const char* const kStatRu[7]  = { "Сил", "Лов", "Тел", "Инт", "Скор", "Хар", "Удч" };
const char* const kStatEn[7]  = { "STR", "DEX", "CON", "INT", "SPD", "CHA", "LCK" };

std::string clean(const std::string& s, int en) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '[') {
            size_t j = s.find(']', i);
            if (j != std::string::npos) {
                std::string tag = s.substr(i + 1, j - i - 1);
                if (tag.compare(0, 4, "img:") == 0) {
                    std::string w = tag.substr(4);
                    const char* lab = nullptr;
                    for (int k = 0; k < 7; ++k) if (w == kStatKey[k]) lab = en ? kStatEn[k] : kStatRu[k];
                    out += lab ? lab : w;
                    i = j + 1;
                    continue;
                }
                // [/?word(:anything)?]
                size_t p = 0;
                if (p < tag.size() && tag[p] == '/') ++p;
                size_t w0 = p;
                while (p < tag.size() && tag[p] >= 'a' && tag[p] <= 'z') ++p;
                if (p > w0 && (p == tag.size() || tag[p] == ':')) { i = j + 1; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}

// --- GON, the way the generators read it: one statement per line ------------
// Calls cb(name, keys, depth_after_pop) for every named block as it closes; a
// child block named `meta` adds its keys to its parent as meta.<key> instead
// (with_meta), which is how abilities keep their name and description.
template <class F>
void gon_blocks(const std::string& text, bool with_meta, F cb) {
    struct Open { std::string name; Keys keys; };
    std::vector<Open> stack;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        size_t cm = line.find("//");
        if (cm != std::string::npos) line.resize(cm);
        int opens = 0, closes = 0;
        for (char c : line) { if (c == '{' || c == '[') ++opens; else if (c == '}' || c == ']') ++closes; }
        std::string t = trim(line);
        // `Name {` alone on its line
        if (!t.empty() && t.back() == '{') {
            std::string head = trim(t.substr(0, t.size() - 1));
            bool ident = !head.empty();
            for (char c : head) if (!is_ident(c)) { ident = false; break; }
            if (ident && opens == 1 && closes == 0) { stack.push_back({ head, {} }); continue; }
        }
        if (opens == 0 && closes == 0) {
            if (!t.empty() && is_ident_start(t[0]) && !stack.empty()) {
                size_t k = 0;
                while (k < t.size() && is_ident(t[k])) ++k;
                if (k < t.size() && isspace((unsigned char)t[k])) {
                    std::string key = t.substr(0, k), val = trim(t.substr(k));
                    if (!val.empty()) stack.back().keys.emplace(key, val);
                }
            }
            continue;
        }
        for (int i = 0; i < opens - closes; ++i) stack.push_back({ "", {} });
        for (int i = 0; i < closes - opens; ++i) {
            if (stack.empty()) break;
            Open o = std::move(stack.back());
            stack.pop_back();
            if (with_meta && o.name == "meta" && !stack.empty()) {
                for (auto& kv : o.keys) stack.back().keys.emplace("meta." + kv.first, kv.second);
            } else if (!o.name.empty()) {
                cb(o.name, o.keys, (int)stack.size());
            }
        }
        if (pos > text.size()) break;
    }
}

Keys resolve(const std::map<std::string, Keys>& raw, const std::string& name, int depth = 0) {
    const Keys& own = raw.at(name);
    auto b = own.find("variant_of");
    if (b != own.end() && raw.count(b->second) && depth < 4) {
        Keys m = resolve(raw, b->second, depth + 1);
        for (auto& kv : own) m[kv.first] = kv.second;
        return m;
    }
    return own;
}

std::string get(const Keys& k, const char* key) {
    auto it = k.find(key);
    return it == k.end() ? std::string() : it->second;
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!isdigit((unsigned char)c)) return false;
    return true;
}
bool is_int(const std::string& s) {
    return !s.empty() && all_digits(s[0] == '-' ? s.substr(1) : s);
}

std::string read_text(const cr::GPak& gp, const std::string& name) {
    std::vector<uint8_t> b;
    if (!gp.read(name, b)) return std::string();
    return std::string(b.begin(), b.end());
}

// make_item_table.py's top-level blocks: `Name {` at the start of a line, body to
// the matching brace; `kind` and `rarity` taken from the first lines that set them.
void item_pool(const cr::GPak& gp) {
    static const char* const kFiles[5] = { "head_items", "face_items", "neck_items", "weapons", "trinkets" };
    static const char* const kSlots[5] = { "head", "face", "neck", "weapon", "trinket" };
    std::set<std::string> per[5];
    for (const char* f : kFiles) {
        std::string t = read_text(gp, std::string("data/items/") + f + ".gon");
        size_t i = 0;
        while (i < t.size()) {
            size_t ls = i;                                  // a line start
            size_t le = t.find('\n', ls);
            if (le == std::string::npos) le = t.size();
            size_t k = ls;
            while (k < le && is_ident(t[k])) ++k;
            size_t sp = k;
            while (sp < le && isspace((unsigned char)t[sp])) ++sp;
            if (k > ls && sp < le && t[sp] == '{') {
                std::string name = t.substr(ls, k - ls);
                int depth = 0;
                size_t j = sp;
                for (; j < t.size(); ++j) {
                    if (t[j] == '{') ++depth;
                    else if (t[j] == '}' && --depth == 0) break;
                }
                std::string body = t.substr(sp + 1, j > sp ? j - sp - 1 : 0);
                std::string kind, rarity;
                size_t bp = 0;
                while (bp < body.size() && (kind.empty() || rarity.empty())) {
                    size_t be = body.find('\n', bp);
                    if (be == std::string::npos) be = body.size();
                    std::string ln = trim(body.substr(bp, be - bp));
                    if (kind.empty() && ln.compare(0, 5, "kind ") == 0) kind = trim(ln.substr(5));
                    if (rarity.empty() && ln.compare(0, 7, "rarity ") == 0) rarity = trim(ln.substr(7));
                    bp = be + 1;
                }
                auto word = [](std::string s) { size_t e = 0; while (e < s.size() && is_ident(s[e])) ++e; return s.substr(0, e); };
                kind = word(kind); rarity = word(rarity);
                bool rar = rarity == "common" || rarity == "uncommon" || rarity == "rare" || rarity == "very_rare";
                for (int s = 0; s < 5 && rar; ++s) if (kind == kSlots[s]) per[s].insert(name);
                i = j + 1;
                continue;
            }
            i = le + 1;
        }
    }
    for (int s = 0; s < 5; ++s) g.slot_pool[s].assign(per[s].begin(), per[s].end());
}

void build() {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) strcpy(slash + 1, "resources.gpak");
    cr::GPak gp;
    if (!gp.open(exe)) { LOG("gamedata: cannot open %s", exe); return; }

    // The translation table: key -> {ru, en}, empty translations fall back to English.
    std::unordered_map<std::string, std::string> text[2];
    {
        auto rows = parse_csv(read_text(gp, "data/text/combined.csv"));
        if (rows.empty()) { LOG("gamedata: no data/text/combined.csv"); return; }
        std::vector<std::string>& head = rows[0];
        if (!head.empty() && head[0].compare(0, 3, "\xEF\xBB\xBF") == 0) head[0].erase(0, 3);
        int en = -1, ru = -1;
        for (int i = 0; i < (int)head.size(); ++i) { if (head[i] == "en") en = i; if (head[i] == "ru") ru = i; }
        if (en < 0) { LOG("gamedata: combined.csv has no 'en' column"); return; }
        for (size_t r = 1; r < rows.size(); ++r) {
            const auto& row = rows[r];
            if (row.empty() || (int)row.size() <= en) continue;
            const std::string& e = row[en];
            text[1][row[0]] = e;
            text[0][row[0]] = (ru >= 0 && (int)row.size() > ru && !row[ru].empty()) ? row[ru] : e;
        }
    }
    auto tr = [&](int en, const std::string& key) -> std::string {
        if (key.empty()) return std::string();
        auto it = text[en].find(strip_quotes(key));
        return it == text[en].end() ? std::string() : clean(it->second, en);
    };

    // Items: every named block in data/items/*.gon at any depth, variant_of resolved.
    {
        std::map<std::string, Keys> raw;
        auto files = gp.list("data/items/", ".gon");
        std::sort(files.begin(), files.end());
        for (auto& f : files)
            gon_blocks(read_text(gp, f), false, [&](const std::string& n, const Keys& k, int) { raw.emplace(n, k); });
        static const std::map<std::string, int> kRarity = { {"common",0}, {"uncommon",1}, {"rare",2}, {"very_rare",3} };
        for (auto& kv : raw) {
            Keys k = resolve(raw, kv.first);
            std::string nk = get(k, "name");
            if (nk.empty()) continue;
            Item it;
            it.id = kv.first;
            it.kind = get(k, "kind");
            for (int en = 0; en < 2; ++en) {
                std::string nm = tr(en, nk);
                it.name[en] = nm.empty() ? it.id : nm;
                it.desc[en] = tr(en, get(k, "desc"));
                std::string st;
                for (int s = 0; s < 7; ++s) {
                    std::string v = get(k, kStatKey[s]);
                    if (is_int(v) && atoi(v.c_str())) {
                        char b[32];
                        snprintf(b, sizeof b, "%s %+d", en ? kStatEn[s] : kStatRu[s], atoi(v.c_str()));
                        if (!st.empty()) st += ", ";
                        st += b;
                    }
                }
                it.stats[en] = st;
            }
            auto rr = kRarity.find(get(k, "rarity"));
            it.rarity = get(k, "cursed") == "true" ? 4 : rr == kRarity.end() ? 4 : rr->second;
            std::string fr = get(k, "frame");
            it.frame = all_digits(fr) ? atoi(fr.c_str()) : -1;
            g.items.push_back(std::move(it));
        }
        std::sort(g.items.begin(), g.items.end(), [](const Item& a, const Item& b) { return strcmp(a.id.c_str(), b.id.c_str()) < 0; });
        for (size_t i = 0; i < g.items.size(); ++i) g.item_index[g.items[i].id] = i;
    }

    // Class names: CAT_CLASS_<ID>_NAME.
    {
        std::set<std::string> ids;
        for (auto& kv : text[1]) {
            const std::string& k = kv.first;
            if (k.size() > 15 && k.compare(0, 10, "CAT_CLASS_") == 0 && k.compare(k.size() - 5, 5, "_NAME") == 0)
                ids.insert(k.substr(10, k.size() - 15));
        }
        for (auto& id : ids) {
            Class c;
            c.id = id;
            for (int en = 0; en < 2; ++en) c.name[en] = text[en]["CAT_CLASS_" + id + "_NAME"];
            g.classes.push_back(std::move(c));
        }
    }

    // Abilities: top-level blocks of data/abilities/*.gon, name/desc in `meta`.
    {
        std::map<std::string, Keys> abil;
        auto files = gp.list("data/abilities/", ".gon");
        std::sort(files.begin(), files.end());
        for (auto& f : files) {
            std::map<std::string, Keys> raw;
            gon_blocks(read_text(gp, f), true, [&](const std::string& n, const Keys& k, int depth) {
                if (depth == 0) raw.emplace(n, k);
            });
            for (auto& kv : raw) abil.emplace(kv.first, kv.second);
        }
        std::function<std::string(const std::string&, int)> root = [&](const std::string& n, int d) -> std::string {
            std::string b = get(abil.at(n), "variant_of");
            return (!b.empty() && abil.count(b) && d < 4) ? root(b, d + 1) : n;
        };
        for (auto& kv : abil) {
            Keys k = resolve(abil, kv.first);
            std::string nm0 = tr(0, get(k, "meta.name"));
            if (nm0.empty()) continue;
            Skill s;
            s.id = kv.first;
            s.base = root(kv.first, 0);
            for (int en = 0; en < 2; ++en) {
                s.name[en] = tr(en, get(k, "meta.name"));
                s.desc[en] = tr(en, get(k, "meta.desc"));
            }
            g.abilities.push_back(std::move(s));
        }
        std::sort(g.abilities.begin(), g.abilities.end(), [](const Skill& a, const Skill& b) { return strcmp(a.id.c_str(), b.id.c_str()) < 0; });
    }

    // Passives: top-level blocks of data/passives/*.gon with a name.
    {
        std::map<std::string, Skill> out;
        auto files = gp.list("data/passives/", ".gon");
        std::sort(files.begin(), files.end());
        for (auto& f : files)
            gon_blocks(read_text(gp, f), true, [&](const std::string& n, const Keys& k, int depth) {
                if (depth != 0 || get(k, "name").empty() || out.count(n)) return;
                Skill s;
                s.id = n;
                s.base = n;
                for (int en = 0; en < 2; ++en) { s.name[en] = tr(en, get(k, "name")); s.desc[en] = tr(en, get(k, "desc")); }
                out.emplace(n, std::move(s));
            });
        for (auto& kv : out) g.passives.push_back(std::move(kv.second));
        std::sort(g.passives.begin(), g.passives.end(), [](const Skill& a, const Skill& b) { return strcmp(a.id.c_str(), b.id.c_str()) < 0; });
    }

    item_pool(gp);

    // The C views, built once now that nothing above will move again.
    for (int en = 0; en < 2; ++en) {
        for (auto& it : g.items)
            g.item_view[en].push_back({ it.id.c_str(), it.name[en].c_str(), it.desc[en].c_str(), it.rarity, it.stats[en].c_str(), it.frame });
        for (auto& c : g.classes) g.class_view[en].push_back({ c.id.c_str(), c.name[en].c_str() });
        for (auto& s : g.abilities) g.abil_view[en].push_back({ s.id.c_str(), s.name[en].c_str(), s.desc[en].c_str(), s.base.c_str() });
        for (auto& s : g.passives)  g.pass_view[en].push_back({ s.id.c_str(), s.name[en].c_str(), s.desc[en].c_str(), s.base.c_str() });
    }
    LOG("gamedata: %zu items, %zu classes, %zu abilities, %zu passives; random pool %zu/%zu/%zu/%zu/%zu",
        g.items.size(), g.classes.size(), g.abilities.size(), g.passives.size(),
        g.slot_pool[0].size(), g.slot_pool[1].size(), g.slot_pool[2].size(), g.slot_pool[3].size(), g.slot_pool[4].size());
    g.ready = true;
}

void worker() {
    try { build(); } catch (...) { LOG("gamedata: stopped by an exception"); }
}

} // namespace

extern "C" void gd_start(gd_log_fn log) {
    if (g.started.exchange(true)) return;
    g.log = log;
    std::thread(worker).detach();
}

extern "C" int gd_ready(void) { return g.ready ? 1 : 0; }

extern "C" int gd_slot_count(int slot) {
    return g.ready && slot >= 0 && slot < 5 ? (int)g.slot_pool[slot].size() : 0;
}
extern "C" const char* gd_slot_item(int slot, int i) {
    if (!g.ready || slot < 0 || slot >= 5 || i < 0 || i >= (int)g.slot_pool[slot].size()) return nullptr;
    return g.slot_pool[slot][i].c_str();
}

template <class T> static const T* view(const std::vector<T>* v, int en, int* n) {
    if (!g.ready) { if (n) *n = 0; return nullptr; }
    const std::vector<T>& x = v[en ? 1 : 0];
    if (n) *n = (int)x.size();
    return x.empty() ? nullptr : x.data();
}
extern "C" const item_text*  gd_items(int en, int* n)     { return view(g.item_view, en, n); }
extern "C" const class_text* gd_classes(int en, int* n)   { return view(g.class_view, en, n); }
extern "C" const skill_text* gd_abilities(int en, int* n) { return view(g.abil_view, en, n); }
extern "C" const skill_text* gd_passives(int en, int* n)  { return view(g.pass_view, en, n); }

extern "C" const char* gd_item_kind(const char* id) {
    if (!g.ready || !id) return "";
    auto it = g.item_index.find(id);
    return it == g.item_index.end() ? "" : g.items[it->second].kind.c_str();
}
extern "C" int gd_item_frame(const char* id) {
    if (!g.ready || !id) return 0;
    auto it = g.item_index.find(id);
    return it == g.item_index.end() || g.items[it->second].frame < 1 ? 0 : g.items[it->second].frame;
}
