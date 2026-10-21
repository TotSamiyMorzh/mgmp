// portraits -- cat faces for the lobby, composed from the game's own art.
//
// The renderer is combat_roster's (mewgenics-mods/combat_roster/src: swf.cpp, gpak.cpp,
// catportrait.cpp, copied into portrait/): swfs/catparts.swf `CatHeadPlacements` with the
// cat's parts on its markers, coloured through a row of textures/palette.png -- the way
// glaiel::CatParts builds a cat. A worker thread loads the art and renders; the GL upload
// happens on the game thread when the lobby asks for the texture.
#pragma once

struct portrait_look {
    int head, ear, eye, brow, mouth, tex;   // 1-based part indices (CatData BodyParts)
    int palette;                            // palette.png row (CatData+0x7C: the cat's own colours)
};

typedef void (*portraits_log_fn)(const char* fmt, ...);

// Starts the worker once (reads <game dir>\resources.gpak). Safe to call repeatedly.
void portraits_start(portraits_log_fn log);
// GL texture of this face at `px` (longer side), or 0 while it is not ready yet.
// Call on the thread that owns the GL context. *w / *h get the image size.
unsigned portraits_tex(const portrait_look& look, int px, int* w, int* h);
// Icon of an active ability (passive = 0) or a passive (1) from swfs/ability_icons.swf:
// the frame of `AbilityIcon` / `PassiveIcon` labelled with the id (combat_roster's
// lookup), else the one labelled `alt` (an upgraded ability's base). 0 = not ready or none.
unsigned portraits_icon(int passive, const char* id, const char* alt, int px);
