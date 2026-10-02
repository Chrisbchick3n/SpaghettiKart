#include "RoomRelay.h"

#include <cstdio>

namespace Netplay {

RoomRelay::~RoomRelay() {
    Stop();
}

bool RoomRelay::Start(uint16_t port, std::string& err) {
    NetInit();
    return mListener.Listen(port, err);
}

void RoomRelay::StartThread() {
    mStop = false;
    mThread = std::thread([this] { Run(); });
}

void RoomRelay::Stop() {
    mStop = true;
    if (mThread.joinable()) {
        mThread.join();
    }
    mPending.clear();
    mRooms.clear();
    mRoomCount = 0;
    mListener.Close();
}

void RoomRelay::Run() {
    while (!mStop) {
        RunOnce(5);
    }
}

size_t RoomRelay::TotalConnections() const {
    size_t n = mPending.size();
    for (auto& kv : mRooms) {
        n += kv.second.server->PeerCount();
    }
    return n;
}

std::string RoomRelay::NewCode() {
    const size_t alphabetSize = sizeof(kRoomCodeAlphabet) - 1;
    std::uniform_int_distribution<size_t> pick(0, alphabetSize - 1);
    while (true) {
        std::string code;
        for (int i = 0; i < kRoomCodeLength; i++) {
            code.push_back(kRoomCodeAlphabet[pick(mRng)]);
        }
        if (mRooms.find(code) == mRooms.end()) {
            return code;
        }
    }
}

void RoomRelay::Reject(Connection& c, const std::string& why) {
    Writer w(S2C_REJECT);
    w.str(why);
    c.Send(w.done());
    c.Flush();
}

void RoomRelay::HandlePending(Pending& p, bool& remove) {
    remove = false;
    bool alive = p.conn->Pump();
    Message m;
    if (!p.conn->PopMessage(m)) {
        if (!alive || std::chrono::steady_clock::now() - p.since > std::chrono::milliseconds(mLimits.pendingTimeoutMs)) {
            remove = true;
        }
        return;
    }
    Reader r(m.payload.data(), m.payload.size());
    remove = true; // from here on the connection either moves into a room or is dropped

    if (m.type == C2S_HELLO) {
        // Older build using plain Join: shared direct room, first to join leads.
        Room& room = mRooms[""];
        if (!room.server) {
            room.server = std::make_unique<Server>();
            room.server->SetLogTag("relay direct");
        }
        p.conn->PushFront(std::move(m));
        room.server->Adopt(std::move(p.conn));
        return;
    }

    if (m.type != C2S_ROOM_CREATE && m.type != C2S_ROOM_JOIN) {
        return; // not a netplay client
    }
    uint16_t proto = r.b16();
    std::string code = m.type == C2S_ROOM_JOIN ? NormalizeRoomCode(r.str()) : std::string();
    if (!r.ok()) {
        return;
    }
    if (proto != kProtocolVersion) {
        Reject(*p.conn, "Your SpaghettiKart netplay version doesn't match the online server. Everyone needs the "
                        "latest netplay build.");
        return;
    }

    if (m.type == C2S_ROOM_CREATE) {
        if (mRooms.size() >= mLimits.maxRooms) {
            Reject(*p.conn, "The online server is full right now. Try again in a few minutes.");
            return;
        }
        code = NewCode();
        Room& room = mRooms[code];
        room.server = std::make_unique<Server>();
        room.server->SetLogTag("room " + code);
        fprintf(stderr, "[relay] room %s created (%zu rooms)\n", code.c_str(), mRooms.size());
    } else {
        auto it = code.empty() ? mRooms.end() : mRooms.find(code);
        if (it == mRooms.end()) {
            Reject(*p.conn, "No game with code " + (code.empty() ? std::string("(empty)") : code) +
                                ". Check the code, and make sure the host is still in the Netplay lobby.");
            return;
        }
    }

    Writer w(S2C_ROOM);
    w.str(code);
    p.conn->Send(w.done());
    // Anything the client already sent after the room request (its HELLO) stays queued in the connection.
    mRooms[code].server->Adopt(std::move(p.conn));
}

void RoomRelay::RunOnce(int waitMs) {
    // New connections
    while (Connection* c = mListener.Accept()) {
        std::unique_ptr<Connection> conn(c);
        if (TotalConnections() >= mLimits.maxConnections) {
            Reject(*conn, "The online server is full right now. Try again in a few minutes.");
            continue;
        }
        mPending.push_back(Pending{ std::move(conn), std::chrono::steady_clock::now() });
    }

    // Connections that haven't picked a room yet
    for (size_t i = 0; i < mPending.size();) {
        bool remove;
        HandlePending(mPending[i], remove);
        if (remove) {
            mPending.erase(mPending.begin() + i);
        } else {
            i++;
        }
    }

    // Rooms
    for (auto it = mRooms.begin(); it != mRooms.end();) {
        it->second.server->Step();
        if (it->second.server->PeerCount() == 0) {
            if (!it->first.empty()) {
                fprintf(stderr, "[relay] room %s closed\n", it->first.c_str());
            }
            it = mRooms.erase(it);
        } else {
            ++it;
        }
    }
    mRoomCount = mRooms.size();

    auto now = std::chrono::steady_clock::now();
    if (now - mLastStats > std::chrono::minutes(10)) {
        mLastStats = now;
        fprintf(stderr, "[relay] %zu room(s), %zu connection(s)\n", mRooms.size(), TotalConnections());
    }

    std::vector<NetSocketHandle> socks{ mListener.Handle() };
    for (auto& p : mPending) {
        socks.push_back(p.conn->Handle());
    }
    for (auto& kv : mRooms) {
        kv.second.server->CollectSockets(socks);
    }
    WaitReadable(socks, waitMs);
}

} // namespace Netplay
