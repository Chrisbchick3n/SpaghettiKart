#pragma once
// SpaghettiKart lockstep netplay - wire protocol.
//
// Every message on the TCP stream is: [u8 type][u32 payload length, little endian][payload].
// All integers are little endian. Strings are [u8 length][bytes].
//
// The model is "input lockstep": every player's game runs the exact same simulation.
// Each frame, every client sends the controller state it wants to use `inputDelay` frames
// in the future. The host (server) waits until it has input from every player for a frame,
// then broadcasts the combined inputs. A client may only simulate frame F once it has
// the combined inputs for F. Nothing else about the game state ever crosses the network,
// apart from a one-time snapshot at session start and periodic desync-check hashes.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Netplay {

constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultPort = 25564;
constexpr int kMaxPlayers = 4;
constexpr int kMinInputDelay = 1;
constexpr int kMaxInputDelay = 10;
constexpr uint32_t kHashInterval = 60; // frames between desync checks (~2 s at 30 fps)
constexpr uint32_t kMaxMessageSize = 1 << 20;

// Online play through a relay server ("Host online" / "Join with code").
// Players only ever connect OUT to the relay, so nobody needs port forwarding or a VPN. The relay runs one
// lockstep Server per room; the first message on a relay connection picks the room:
//   C2S_ROOM_CREATE -> relay makes a new room and answers S2C_ROOM with its code
//   C2S_ROOM_JOIN   -> relay answers S2C_ROOM (same code) or S2C_REJECT ("no room with that code")
// After that, the connection is a normal netplay connection (C2S_HELLO, lobby, session...).
// The relay address baked into official builds comes from the NETPLAY_DEFAULT_RELAY compile definition
// ("host" or "host:port"); players can override it in the Netplay window (Advanced).
#ifndef NETPLAY_DEFAULT_RELAY
#define NETPLAY_DEFAULT_RELAY ""
#endif
constexpr const char* kDefaultRelay = NETPLAY_DEFAULT_RELAY;
constexpr int kRoomCodeLength = 5;
// No 0/O, 1/I/L, 5/S, 2/Z: codes get read out over voice chat.
constexpr const char kRoomCodeAlphabet[] = "ABCDEFGHJKMNPQRTUVWXY346789";

// Uppercases and strips anything that isn't a letter or digit ("abc-de " -> "ABCDE").
inline std::string NormalizeRoomCode(const std::string& in) {
    std::string out;
    for (char ch : in) {
        if (ch >= 'a' && ch <= 'z') {
            ch = (char) (ch - 'a' + 'A');
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            out.push_back(ch);
        }
    }
    return out;
}

// Splits "host", "host:port" or "[v6addr]:port". Bare IPv6 addresses are left alone.
inline void SplitHostPort(const std::string& in, std::string& host, uint16_t& port, uint16_t defaultPort) {
    host = in;
    port = defaultPort;
    while (!host.empty() && host.back() == ' ') {
        host.pop_back();
    }
    while (!host.empty() && host.front() == ' ') {
        host.erase(host.begin());
    }
    if (!host.empty() && host.front() == '[') {
        size_t close = host.find(']');
        if (close != std::string::npos) {
            std::string rest = host.substr(close + 1);
            host = host.substr(1, close - 1);
            if (rest.size() > 1 && rest[0] == ':') {
                int p = atoi(rest.c_str() + 1);
                if (p > 0 && p < 65536) {
                    port = (uint16_t) p;
                }
            }
        }
        return;
    }
    size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) {
        int p = atoi(host.c_str() + colon + 1);
        if (p > 0 && p < 65536) {
            port = (uint16_t) p;
        }
        host = host.substr(0, colon);
    }
}

enum MsgType : uint8_t {
    // client -> server
    C2S_HELLO = 1,     // u16 protocol, str build, str name
    C2S_START_REQ = 2, // (leader only) u8 inputDelay, u32 snapshotLen, bytes snapshot
    C2S_INPUT = 3,     // u32 frame, Pad
    C2S_HASH = 4,      // u32 frame, u32 hash
    C2S_CHAT = 5,      // str text
    C2S_END_REQ = 6,   // (leader only) end the session, everyone returns to the lobby
    C2S_ROOM_CREATE = 7, // (relay only, first message) u16 protocol
    C2S_ROOM_JOIN = 8,   // (relay only, first message) u16 protocol, str code

    // server -> client
    S2C_WELCOME = 64,     // u8 yourSlot
    S2C_REJECT = 65,      // str reason
    S2C_LOBBY = 66,       // u8 count, count x (u8 slot, str name)
    S2C_START = 67,       // u8 numPlayers, u8 yourController, u8 inputDelay, u32 snapshotLen, bytes snapshot
    S2C_INPUTS = 68,      // u32 frame, u8 numPlayers, numPlayers x Pad
    S2C_DESYNC = 69,      // u32 frame
    S2C_PLAYER_LEFT = 70, // u8 slot, str name   (their controller goes neutral, race continues)
    S2C_CHAT = 71,        // u8 slot, str text
    S2C_SESSION_END = 72, // str reason
    S2C_ROOM = 73,        // str code   (relay: you are in this room)
};

// One controller's state for one frame. 8 bytes on the wire.
struct Pad {
    uint16_t button = 0;
    int8_t stickX = 0;
    int8_t stickY = 0;
    int8_t rightStickX = 0;
    int8_t rightStickY = 0;
    bool operator==(const Pad& o) const {
        return button == o.button && stickX == o.stickX && stickY == o.stickY && rightStickX == o.rightStickX &&
               rightStickY == o.rightStickY;
    }
};
constexpr size_t kPadWireSize = 8;

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------
class Writer {
  public:
    explicit Writer(uint8_t type) {
        mBuf.push_back(type);
        mBuf.resize(5); // room for length
    }
    Writer& b8(uint8_t v) {
        mBuf.push_back(v);
        return *this;
    }
    Writer& b16(uint16_t v) {
        mBuf.push_back(v & 0xFF);
        mBuf.push_back(v >> 8);
        return *this;
    }
    Writer& b32(uint32_t v) {
        for (int i = 0; i < 4; i++) {
            mBuf.push_back((v >> (8 * i)) & 0xFF);
        }
        return *this;
    }
    Writer& str(const std::string& s) {
        size_t n = s.size() > 255 ? 255 : s.size();
        b8((uint8_t) n);
        mBuf.insert(mBuf.end(), s.begin(), s.begin() + n);
        return *this;
    }
    Writer& bytes(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*) p;
        mBuf.insert(mBuf.end(), b, b + n);
        return *this;
    }
    Writer& blob(const std::vector<uint8_t>& v) {
        b32((uint32_t) v.size());
        return bytes(v.data(), v.size());
    }
    Writer& pad(const Pad& p) {
        b16(p.button);
        b8((uint8_t) p.stickX);
        b8((uint8_t) p.stickY);
        b8((uint8_t) p.rightStickX);
        b8((uint8_t) p.rightStickY);
        return b16(0); // reserved
    }
    // Finalize: fills in the length field and returns the full message.
    const std::vector<uint8_t>& done() {
        uint32_t len = (uint32_t) (mBuf.size() - 5);
        for (int i = 0; i < 4; i++) {
            mBuf[1 + i] = (len >> (8 * i)) & 0xFF;
        }
        return mBuf;
    }

  private:
    std::vector<uint8_t> mBuf;
};

class Reader {
  public:
    Reader(const uint8_t* p, size_t n) : mP(p), mN(n) {
    }
    bool ok() const {
        return mOk;
    }
    uint8_t b8() {
        if (!need(1)) {
            return 0;
        }
        return mP[mI++];
    }
    uint16_t b16() {
        if (!need(2)) {
            return 0;
        }
        uint16_t v = mP[mI] | (mP[mI + 1] << 8);
        mI += 2;
        return v;
    }
    uint32_t b32() {
        if (!need(4)) {
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            v |= (uint32_t) mP[mI + i] << (8 * i);
        }
        mI += 4;
        return v;
    }
    std::string str() {
        uint8_t n = b8();
        if (!need(n)) {
            return {};
        }
        std::string s((const char*) mP + mI, n);
        mI += n;
        return s;
    }
    std::vector<uint8_t> blob() {
        uint32_t n = b32();
        if (!need(n)) {
            return {};
        }
        std::vector<uint8_t> v(mP + mI, mP + mI + n);
        mI += n;
        return v;
    }
    Pad pad() {
        Pad p;
        p.button = b16();
        p.stickX = (int8_t) b8();
        p.stickY = (int8_t) b8();
        p.rightStickX = (int8_t) b8();
        p.rightStickY = (int8_t) b8();
        b16(); // reserved
        return p;
    }

  private:
    bool need(size_t n) {
        if (!mOk || mI + n > mN) {
            mOk = false;
            return false;
        }
        return true;
    }
    const uint8_t* mP;
    size_t mN;
    size_t mI = 0;
    bool mOk = true;
};

// FNV-1a, used for desync-check hashes and the settings fingerprint.
struct Hasher {
    uint32_t h = 2166136261u;
    void add(const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*) p;
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 16777619u;
        }
    }
    template <typename T> void add(const T& v) {
        add(&v, sizeof(T));
    }
};

} // namespace Netplay
