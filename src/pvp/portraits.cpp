#include "portraits.h"

#include "portrait/catportrait.h"
#include "portrait/gpak.h"
#include "portrait/swf.h"

#include <windows.h>
#include <GL/gl.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// stb_image's implementation lives in mgmp_overlay.cpp (no stdio).
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace {

struct Face {
    int state = 0;            // 0 queued, 1 pixels ready, 2 uploaded, -1 failed
    int kind = 0;             // 0 cat face, 1 ability icon, 2 passive icon
    std::string id, alt;
    cr::CatLook look;
    int px = 0;
    cr::SwfImage img;
    GLuint tex = 0;
};

struct State {
    std::atomic<bool> started{false}, ready{false};
    portraits_log_fn log = nullptr;
    cr::GPak gpak;
    cr::SwfDoc catparts, icons;
    std::vector<uint8_t> palette;   // 256 rows x 16 RGB
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<std::string, Face> faces;
    std::deque<std::string> queue;
} g;

#define LOG(...) do { if (g.log) g.log(__VA_ARGS__); } while (0)

void worker_body() {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) strcpy(slash + 1, "resources.gpak");
    if (!g.gpak.open(exe)) { LOG("portraits: cannot open %s", exe); return; }
    std::vector<uint8_t> buf;
    bool cp = g.gpak.read("swfs/catparts.swf", buf) && g.catparts.load(std::move(buf));
    g.catparts.hide_instances({"sloticon", "label"});
    bool ai = g.gpak.read("swfs/ability_icons.swf", buf) && g.icons.load(std::move(buf));
    g.icons.hide_instances({"sloticon", "label"});
    LOG("portraits: ability_icons.swf %s", ai ? "ok" : "FAILED");
    std::vector<uint8_t> png;
    int w = 0, h = 0, n = 0;
    if (g.gpak.read("textures/palette.png", png))
        if (uint8_t* p = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &n, 3)) {
            if (w == 16) g.palette.assign(p, p + (size_t)w * h * 3);
            stbi_image_free(p);
        }
    LOG("portraits: catparts.swf %s, palette %zu rows", cp ? "ok" : "FAILED", g.palette.size() / 48);
    if (!cp || g.palette.empty()) return;
    g.ready = true;
    for (;;) {
        std::string key, id, alt;
        cr::CatLook look;
        int px, kind;
        {
            std::unique_lock<std::mutex> lk(g.mu);
            g.cv.wait(lk, [] { return !g.queue.empty(); });
            key = g.queue.front();
            g.queue.pop_front();
            Face& f = g.faces[key];
            look = f.look;
            px = f.px;
            kind = f.kind; id = f.id; alt = f.alt;
        }
        cr::SwfImage im;
        bool ok = false;
        if (kind) {
            const char* sym = kind == 1 ? "AbilityIcon" : "PassiveIcon";
            try {
                int fr = g.icons.frame_of_label(sym, id);
                if (fr < 0 && !alt.empty()) fr = g.icons.frame_of_label(sym, alt);
                ok = fr >= 0 && g.icons.render(sym, fr, px, im);
            } catch (...) { ok = false; }
            std::lock_guard<std::mutex> lk(g.mu);
            Face& f = g.faces[key];
            if (ok) { f.img = std::move(im); f.state = 1; } else f.state = -1;
            continue;
        }
        int row = look.palette;
        if (row < 0 || (size_t)row * 48 >= g.palette.size()) row = 0;
        try {
            cr::SwfRenderOpts o = cr::cat_face_opts(look, &g.palette[(size_t)row * 48]);
            ok = g.catparts.render_ex("CatHeadPlacements", look.head - 1, px, o, im);
        } catch (...) {
            ok = false;
        }
        std::lock_guard<std::mutex> lk(g.mu);
        Face& f = g.faces[key];
        if (ok) { f.img = std::move(im); f.state = 1; }
        else f.state = -1;
    }
}

// An exception escaping a std::thread would std::terminate the game.
void worker_main() {
    try { worker_body(); } catch (...) { LOG("portraits: worker stopped by an exception"); }
}

}  // namespace

void portraits_start(portraits_log_fn log) {
    if (g.started.exchange(true)) return;
    g.log = log;
    std::thread(worker_main).detach();
}

static unsigned upload(Face& f) {
    if (f.state == 1) {
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glGenTextures(1, &f.tex);
        glBindTexture(GL_TEXTURE_2D, f.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f.img.w, f.img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.img.rgba.data());
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        f.state = 2;
    }
    return f.state == 2 ? f.tex : 0;
}

unsigned portraits_icon(int passive, const char* id, const char* alt, int px) {
    if (!g.ready || !id || !id[0]) return 0;
    std::string key = std::string(passive ? "P|" : "A|") + id + "|" + std::to_string(px);
    std::lock_guard<std::mutex> lk(g.mu);
    auto it = g.faces.find(key);
    if (it == g.faces.end()) {
        Face f;
        f.kind = passive ? 2 : 1;
        f.id = id;
        f.alt = alt ? alt : "";
        f.px = px;
        g.faces.emplace(key, std::move(f));
        g.queue.push_back(key);
        g.cv.notify_one();
        return 0;
    }
    return upload(it->second);
}

unsigned portraits_tex(const portrait_look& l, int px, int* w, int* h) {
    if (!g.ready || l.head <= 0) return 0;
    cr::CatLook look;
    look.head = l.head; look.ear = l.ear; look.eye = l.eye; look.brow = l.brow;
    look.mouth = l.mouth; look.tex = l.tex; look.palette = l.palette;
    std::string key = look.key() + "|" + std::to_string(px);
    std::lock_guard<std::mutex> lk(g.mu);
    auto it = g.faces.find(key);
    if (it == g.faces.end()) {
        Face f;
        f.look = look;
        f.px = px;
        g.faces.emplace(key, std::move(f));
        g.queue.push_back(key);
        g.cv.notify_one();
        return 0;
    }
    Face& f = it->second;
    if (f.state == 1) {   // upload on this (GL) thread
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glGenTextures(1, &f.tex);
        glBindTexture(GL_TEXTURE_2D, f.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, f.img.w, f.img.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.img.rgba.data());
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
        f.state = 2;
    }
    if (f.state != 2) return 0;
    if (w) *w = f.img.w;
    if (h) *h = f.img.h;
    return f.tex;
}
