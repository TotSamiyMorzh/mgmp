// mgmp_steam.h -- the Steam side of the transport: SteamNetworkingMessages for
// the bytes, Steam friends for finding each other.
//
// No port forwarding and no IP: peers are addressed by SteamID, Steam punches
// through NAT and falls back to Valve's relays. The game has already called
// SteamAPI_Init (it links steam_api64.dll and runs its callbacks every frame),
// so everything here resolves the flat exports of that same DLL and rides on the
// game's own Steam session. Nothing is linked against the Steamworks SDK.
//
// Finding each other:
//  - a host publishes rich presence "connect" = "+mgmp_join <its SteamID>", so
//    the Steam friends list offers "Join Game" on it, and the panel lists every
//    friend whose presence says so;
//  - a host can invite a friend (ISteamFriends::InviteUserToGame with the same
//    connect string);
//  - either way the joining peer gets GameRichPresenceJoinRequested_t (or the
//    panel click) and the session layer dials "steam:<SteamID>".
#pragma once
#include <cstdint>

namespace mgmp {

// Resolves the exports and registers the callbacks. Safe to call every frame;
// does the work once, and only once the game's SteamAPI is up. False while Steam
// is not available (no DLL, not logged on).
bool steam_ready();

uint64_t    steam_self_id();
const char* steam_self_name();       // UTF-8, "" when unknown

// Reliable, ordered, one message per call. False when Steam refused it.
bool steam_send(uint64_t peer, const void* data, uint32_t len);

// Drains received messages, calling fn(peer, data, len, ctx) for each. The data
// is only valid during the call.
using SteamRecvFn = void (*)(uint64_t peer, const uint8_t* data, uint32_t len, void* ctx);
void steam_poll(SteamRecvFn fn, void* ctx);

// The session with a peer has been closed or has failed (ClosedByPeer or
// ProblemDetectedLocally). A session that is still connecting is not "closed".
bool steam_session_closed(uint64_t peer);
void steam_close(uint64_t peer);

// Hosting: accept incoming sessions and publish the "Join Game" presence.
void steam_set_hosting(bool on);
bool steam_hosting();

// A join the player asked for through Steam (Join Game / an accepted invite):
// returns true once, with the host's SteamID.
bool steam_take_join(uint64_t* host);

// Friends who are running this game right now. `hosting` is set when the
// friend's rich presence carries our connect string (they can be joined).
struct SteamFriend {
    uint64_t id;
    char     name[64];
    bool     hosting;
};
int  steam_friends_in_game(SteamFriend* out, int cap);
bool steam_invite(uint64_t friend_id);

// "steam:<id64>" -> id, 0 when the text is not a Steam address.
uint64_t steam_parse_addr(const char* addr);

} // namespace mgmp
