// mgmp_steam.cpp -- see mgmp_steam.h. Flat Steamworks API, resolved by name from
// the game's own steam_api64.dll.

#include "mgmp_steam.h"
#include "mgmp_log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace mgmp {
namespace {

constexpr uint32_t kAppId          = 686060;   // Mewgenics
constexpr int      kChannel        = 0;
constexpr int      kSendReliable   = 8;        // k_nSteamNetworkingSend_Reliable
constexpr int      kSendAutoRestart= 32;       // k_nSteamNetworkingSend_AutoRestartBrokenSession
constexpr int      kFriendFlagImmediate = 4;   // k_EFriendFlagImmediate
constexpr int      kCbSessionRequest = 1251;   // SteamNetworkingMessagesSessionRequest_t
constexpr int      kCbJoinRequested  = 337;    // GameRichPresenceJoinRequested_t
constexpr const char* kConnectPrefix = "+mgmp_join ";

// SteamNetworkingIdentity is 136 bytes (type, size, 128-byte union). Only ever
// filled and read through the DLL's own helpers; the slack is free insurance.
struct Identity { uint8_t raw[160]; };

// The flat API.
using fn_iface      = void* (*)();
using fn_send       = int  (*)(void*, const Identity*, const void*, uint32_t, int, int);
using fn_recv       = int  (*)(void*, int, void**, int);
using fn_accept     = bool (*)(void*, const void*);
using fn_close_sess = bool (*)(void*, const Identity*);
using fn_sess_info  = int  (*)(void*, const Identity*, void*, void*);
using fn_msg_release= void (*)(void*);
using fn_id_set64   = void (*)(Identity*, uint64_t);
using fn_id_get64   = uint64_t (*)(const void*);
using fn_get_steamid= uint64_t (*)(void*);
using fn_logged_on  = bool (*)(void*);
using fn_set_rp     = bool (*)(void*, const char*, const char*);
using fn_get_rp     = const char* (*)(void*, uint64_t, const char*);
using fn_clear_rp   = void (*)(void*);
using fn_invite     = bool (*)(void*, uint64_t, const char*);
using fn_friend_cnt = int  (*)(void*, int);
using fn_friend_at  = uint64_t (*)(void*, int, int);
using fn_friend_game= bool (*)(void*, uint64_t, void*);
using fn_friend_name= const char* (*)(void*, uint64_t);
using fn_my_name    = const char* (*)(void*);
using fn_relay      = void (*)(void*);
using fn_register   = void (*)(void*, int);

struct Api {
    fn_send send; fn_recv recv; fn_accept accept; fn_close_sess close_sess;
    fn_sess_info sess_info; fn_msg_release msg_release;
    fn_id_set64 id_set64; fn_id_get64 id_get64;
    fn_get_steamid get_steamid; fn_logged_on logged_on;
    fn_set_rp set_rp; fn_get_rp get_rp; fn_clear_rp clear_rp; fn_invite invite;
    fn_friend_cnt friend_cnt; fn_friend_at friend_at; fn_friend_game friend_game;
    fn_friend_name friend_name; fn_my_name my_name;
    fn_relay relay; fn_register reg;
    void* msgs = nullptr; void* friends = nullptr; void* user = nullptr; void* utils = nullptr;
};

// The CCallbackBase layout Steam expects (steam_api_common.h): vtable with the
// two Run overloads and GetCallbackSizeBytes, then a flags byte and the id.
// Declared in the same order as Valve's header and built by the same compiler
// family, so MSVC lays the vtable out exactly as steam_api64.dll expects.
class CallbackBase {
public:
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool io_failure, uint64_t call) = 0;
    virtual int  GetCallbackSizeBytes() = 0;
    uint8_t m_nCallbackFlags = 0;
    int     m_iCallback = 0;
};

struct State {
    bool     tried = false, ok = false;
    Api      api{};
    uint64_t self = 0;
    char     self_name[64] = {};
    bool     hosting = false;
    volatile LONG join_pending = 0;
    uint64_t join_host = 0;
} g;

void accept_session(void* identity);
void join_requested(uint64_t from, const char* connect);

class SessionRequestCb : public CallbackBase {
public:
    void Run(void* p) override { accept_session(p); }
    void Run(void* p, bool, uint64_t) override { accept_session(p); }
    int  GetCallbackSizeBytes() override { return 136; }
};

class JoinRequestedCb : public CallbackBase {
public:
    void Run(void* p) override {
        uint64_t from = 0;
        memcpy(&from, p, sizeof(from));
        join_requested(from, (const char*)p + 8);
    }
    void Run(void* p, bool, uint64_t) override { Run(p); }
    int  GetCallbackSizeBytes() override { return 8 + 256; }
};

SessionRequestCb g_cb_session;
JoinRequestedCb  g_cb_join;

void accept_session(void* identity) {
    if (!g.ok) return;
    const uint64_t who = g.api.id_get64(identity);
    if (!g.hosting) {
        log_line("STEAM", "session request from %llu ignored -- not hosting",
                 (unsigned long long)who);
        return;
    }
    bool ok = g.api.accept(g.api.msgs, identity);
    log_line("STEAM", "session request from %llu %s", (unsigned long long)who,
             ok ? "accepted" : "could NOT be accepted");
}

void join_requested(uint64_t from, const char* connect) {
    // The connect string is ours: "+mgmp_join <host id>". Anything else (some
    // other launch string) is not a join we know how to make.
    const size_t pl = strlen(kConnectPrefix);
    if (!connect || strncmp(connect, kConnectPrefix, pl) != 0) {
        log_line("STEAM", "join request from %llu with an unknown connect string '%s'",
                 (unsigned long long)from, connect ? connect : "");
        return;
    }
    uint64_t host = _strtoui64(connect + pl, nullptr, 10);
    if (!host) return;
    g.join_host = host;
    InterlockedExchange(&g.join_pending, 1);
    log_line("STEAM", "join requested through Steam: host %llu (asked via %llu)",
             (unsigned long long)host, (unsigned long long)from);
}

template <class T> bool resolve(HMODULE dll, T& out, const char* name) {
    out = (T)GetProcAddress(dll, name);
    if (!out) log_line("STEAM", "!! steam_api64.dll has no %s", name);
    return out != nullptr;
}

bool init() {
    HMODULE dll = GetModuleHandleW(L"steam_api64.dll");
    if (!dll) return false;
    Api& a = g.api;
    fn_iface msgs_v = nullptr, friends_v = nullptr, user_v = nullptr, utils_v = nullptr;
    bool ok = resolve(dll, msgs_v,   "SteamAPI_SteamNetworkingMessages_SteamAPI_v002")
           & resolve(dll, friends_v, "SteamAPI_SteamFriends_v018")
           & resolve(dll, user_v,    "SteamAPI_SteamUser_v023")
           & resolve(dll, utils_v,   "SteamAPI_SteamNetworkingUtils_SteamAPI_v004")
           & resolve(dll, a.send,        "SteamAPI_ISteamNetworkingMessages_SendMessageToUser")
           & resolve(dll, a.recv,        "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel")
           & resolve(dll, a.accept,      "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser")
           & resolve(dll, a.close_sess,  "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser")
           & resolve(dll, a.sess_info,   "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo")
           & resolve(dll, a.msg_release, "SteamAPI_SteamNetworkingMessage_t_Release")
           & resolve(dll, a.id_set64,    "SteamAPI_SteamNetworkingIdentity_SetSteamID64")
           & resolve(dll, a.id_get64,    "SteamAPI_SteamNetworkingIdentity_GetSteamID64")
           & resolve(dll, a.get_steamid, "SteamAPI_ISteamUser_GetSteamID")
           & resolve(dll, a.logged_on,   "SteamAPI_ISteamUser_BLoggedOn")
           & resolve(dll, a.set_rp,      "SteamAPI_ISteamFriends_SetRichPresence")
           & resolve(dll, a.get_rp,      "SteamAPI_ISteamFriends_GetFriendRichPresence")
           & resolve(dll, a.clear_rp,    "SteamAPI_ISteamFriends_ClearRichPresence")
           & resolve(dll, a.invite,      "SteamAPI_ISteamFriends_InviteUserToGame")
           & resolve(dll, a.friend_cnt,  "SteamAPI_ISteamFriends_GetFriendCount")
           & resolve(dll, a.friend_at,   "SteamAPI_ISteamFriends_GetFriendByIndex")
           & resolve(dll, a.friend_game, "SteamAPI_ISteamFriends_GetFriendGamePlayed")
           & resolve(dll, a.friend_name, "SteamAPI_ISteamFriends_GetFriendPersonaName")
           & resolve(dll, a.my_name,     "SteamAPI_ISteamFriends_GetPersonaName")
           & resolve(dll, a.relay,       "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess")
           & resolve(dll, a.reg,         "SteamAPI_RegisterCallback");
    if (!ok) return false;

    a.msgs = msgs_v(); a.friends = friends_v(); a.user = user_v(); a.utils = utils_v();
    if (!a.msgs || !a.friends || !a.user || !a.utils) {
        log_line("STEAM", "!! Steam interfaces are not available yet (SteamAPI not initialised?)");
        return false;
    }
    if (!a.logged_on(a.user)) {
        log_line("STEAM", "!! Steam is running but not logged on -- Steam networking is off");
        return false;
    }
    g.self = a.get_steamid(a.user);
    const char* n = a.my_name(a.friends);
    strncpy_s(g.self_name, n ? n : "", _TRUNCATE);

    g_cb_session.m_iCallback = kCbSessionRequest;
    a.reg(&g_cb_session, kCbSessionRequest);
    g_cb_join.m_iCallback = kCbJoinRequested;
    a.reg(&g_cb_join, kCbJoinRequested);

    a.relay(a.utils);   // start talking to the relay network now, not on the first send
    log_line("STEAM", "Steam networking ready -- this player is %llu",
             (unsigned long long)g.self);
    return true;
}

Identity identity_of(uint64_t id) {
    Identity i{};
    g.api.id_set64(&i, id);
    return i;
}

} // namespace

bool steam_ready() {
    if (!g.tried) {
        g.tried = true;
        g.ok = init();
    }
    return g.ok;
}

uint64_t    steam_self_id()   { return steam_ready() ? g.self : 0; }
const char* steam_self_name() { return steam_ready() ? g.self_name : ""; }

bool steam_send(uint64_t peer, const void* data, uint32_t len) {
    if (!steam_ready() || !peer || !len) return false;
    Identity id = identity_of(peer);
    int r = g.api.send(g.api.msgs, &id, data, len, kSendReliable | kSendAutoRestart, kChannel);
    if (r != 1) {   // k_EResultOK
        static int said = 0;
        if (said++ < 5) log_line("STEAM", "!! send to %llu failed (EResult %d)",
                                 (unsigned long long)peer, r);
        return false;
    }
    return true;
}

void steam_poll(SteamRecvFn fn, void* ctx) {
    if (!steam_ready()) return;
    void* msgs[32];
    for (;;) {
        int n = g.api.recv(g.api.msgs, kChannel, msgs, 32);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) {
            // SteamNetworkingMessage_t: m_pData @0, m_cbSize @8, m_identityPeer @16.
            const uint8_t* m = (const uint8_t*)msgs[i];
            const uint8_t* data = *(const uint8_t* const*)m;
            const int      size = *(const int*)(m + 8);
            const uint64_t from = g.api.id_get64(m + 16);
            if (data && size > 0) fn(from, data, (uint32_t)size, ctx);
            g.api.msg_release(msgs[i]);
        }
        if (n < 32) break;
    }
}

bool steam_session_closed(uint64_t peer) {
    if (!steam_ready() || !peer) return true;
    Identity id = identity_of(peer);
    int st = g.api.sess_info(g.api.msgs, &id, nullptr, nullptr);
    return st == 4 || st == 5;   // ClosedByPeer, ProblemDetectedLocally
}

void steam_close(uint64_t peer) {
    if (!steam_ready() || !peer) return;
    Identity id = identity_of(peer);
    g.api.close_sess(g.api.msgs, &id);
}

void steam_set_hosting(bool on) {
    if (!steam_ready()) return;
    g.hosting = on;
    if (on) {
        char connect[64];
        _snprintf_s(connect, sizeof(connect), _TRUNCATE, "%s%llu", kConnectPrefix,
                    (unsigned long long)g.self);
        g.api.set_rp(g.api.friends, "connect", connect);
        g.api.set_rp(g.api.friends, "status", "Mewgenics PvP: hosting");
        log_line("STEAM", "hosting over Steam -- friends can join this player (%s)", connect);
    } else {
        g.api.clear_rp(g.api.friends);
    }
}

bool steam_hosting() { return g.hosting; }

bool steam_take_join(uint64_t* host) {
    if (!InterlockedExchange(&g.join_pending, 0)) return false;
    if (host) *host = g.join_host;
    return true;
}

int steam_friends_in_game(SteamFriend* out, int cap) {
    if (!steam_ready() || !out || cap <= 0) return 0;
    const int total = g.api.friend_cnt(g.api.friends, kFriendFlagImmediate);
    const size_t pl = strlen(kConnectPrefix);
    int n = 0;
    for (int i = 0; i < total && n < cap; ++i) {
        uint64_t id = g.api.friend_at(g.api.friends, i, kFriendFlagImmediate);
        uint8_t info[32] = {};   // FriendGameInfo_t: CGameID m_gameID first
        if (!g.api.friend_game(g.api.friends, id, info)) continue;
        uint64_t game = 0;
        memcpy(&game, info, sizeof(game));
        if ((uint32_t)(game & 0xFFFFFF) != kAppId) continue;
        SteamFriend& f = out[n++];
        f.id = id;
        const char* nm = g.api.friend_name(g.api.friends, id);
        strncpy_s(f.name, nm ? nm : "?", _TRUNCATE);
        const char* c = g.api.get_rp(g.api.friends, id, "connect");
        f.hosting = c && strncmp(c, kConnectPrefix, pl) == 0;
    }
    return n;
}

bool steam_invite(uint64_t friend_id) {
    if (!steam_ready() || !g.hosting) return false;
    char connect[64];
    _snprintf_s(connect, sizeof(connect), _TRUNCATE, "%s%llu", kConnectPrefix,
                (unsigned long long)g.self);
    bool ok = g.api.invite(g.api.friends, friend_id, connect);
    log_line("STEAM", "invite to %llu %s", (unsigned long long)friend_id, ok ? "sent" : "FAILED");
    return ok;
}

uint64_t steam_parse_addr(const char* addr) {
    if (!addr || _strnicmp(addr, "steam:", 6) != 0) return 0;
    return _strtoui64(addr + 6, nullptr, 10);
}

} // namespace mgmp
