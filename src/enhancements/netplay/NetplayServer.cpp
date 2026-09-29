#include "NetplayServer.h"

#include <chrono>
#include <cstdio>

namespace Netplay {

Server::~Server() {
    Stop();
}

bool Server::Start(uint16_t port, std::string& err) {
    Stop();
    if (!mListener.Listen(port, err)) {
        return false;
    }
    mPort = port;
    mStopRequested = false;
    mRunning = true;
    {
        std::lock_guard<std::mutex> lk(mStatusMutex);
        mStatus = Status{};
    }
    Log("Hosting on port " + std::to_string(port));
    mThread = std::thread([this] { Run(); });
    return true;
}

void Server::Stop() {
    mStopRequested = true;
    if (mThread.joinable()) {
        mThread.join();
    }
    mListener.Close();
    mPeers.clear();
    mInSession = false;
    mRunning = false;
}

Server::Status Server::GetStatus() {
    std::lock_guard<std::mutex> lk(mStatusMutex);
    return mStatus;
}

void Server::Log(const std::string& s) {
    std::lock_guard<std::mutex> lk(mStatusMutex);
    mStatus.log.push_back(s);
    if (mStatus.log.size() > 200) {
        mStatus.log.erase(mStatus.log.begin());
    }
    fprintf(stderr, "[netplay-host] %s\n", s.c_str());
}

size_t Server::ActiveCount() const {
    size_t n = 0;
    for (auto& p : mPeers) {
        if (p->hello && !p->dropped) {
            n++;
        }
    }
    return n;
}

void Server::Broadcast(const std::vector<uint8_t>& framed) {
    for (auto& p : mPeers) {
        if (p->hello && !p->dropped) {
            p->conn->Send(framed);
        }
    }
}

void Server::BroadcastLobby() {
    Writer w(S2C_LOBBY);
    std::vector<Peer*> hello;
    for (auto& p : mPeers) {
        if (p->hello) {
            hello.push_back(p.get());
        }
    }
    w.b8((uint8_t) hello.size());
    for (size_t i = 0; i < hello.size(); i++) {
        w.b8((uint8_t) i).str(hello[i]->name);
    }
    Broadcast(w.done());
    std::lock_guard<std::mutex> lk(mStatusMutex);
    mStatus.players.clear();
    for (Peer* p : hello) {
        mStatus.players.push_back(p->name);
    }
    mStatus.inSession = mInSession;
}

void Server::Run() {
    while (!mStopRequested) {
        // Accept new connections
        while (Connection* c = mListener.Accept()) {
            auto p = std::make_unique<Peer>();
            p->conn.reset(c);
            mPeers.push_back(std::move(p));
        }

        std::vector<NetSocketHandle> socks{ mListener.Handle() };
        for (size_t i = 0; i < mPeers.size(); i++) {
            Peer& p = *mPeers[i];
            if (p.dropped) {
                continue; // placeholder for a player who left mid-session
            }
            bool alive = p.conn->Pump();
            Message m;
            while (p.conn->PopMessage(m)) {
                HandleMessage(i, m);
            }
            if (!alive) {
                if (RemovePeer(i, "disconnected")) {
                    i--;
                }
                continue;
            }
            socks.push_back(p.conn->Handle());
        }
        WaitReadable(socks, 2);
    }
}

// Returns true if the peer was erased from mPeers.
bool Server::RemovePeer(size_t idx, const std::string& why) {
    Peer& p = *mPeers[idx];
    std::string name = p.name.empty() ? "someone" : p.name;
    if (p.hello) {
        Log(name + " " + why);
    }
    if (mInSession && p.hello && p.controller >= 0) {
        // Keep the slot in the session with neutral input so the race can continue for everyone else.
        p.dropped = true;
        p.conn->Close();
        Writer w(S2C_PLAYER_LEFT);
        w.b8((uint8_t) p.controller).str(name);
        Broadcast(w.done());
        if (ActiveCount() == 0) {
            EndSession("everyone left"); // this erases dropped placeholders, including this one
            return true;
        }
        TryAdvance();
        return false;
    }
    mPeers.erase(mPeers.begin() + idx);
    BroadcastLobby();
    return true;
}

void Server::EndSession(const std::string& why) {
    Log("Session ended: " + why);
    Writer w(S2C_SESSION_END);
    w.str(why);
    Broadcast(w.done());
    mInSession = false;
    mHashes.clear();
    // Drop the placeholders of players who left
    for (size_t i = 0; i < mPeers.size();) {
        if (mPeers[i]->dropped) {
            mPeers.erase(mPeers.begin() + i);
        } else {
            mPeers[i]->controller = -1;
            mPeers[i]->inputs.clear();
            i++;
        }
    }
    BroadcastLobby();
}

void Server::HandleMessage(size_t idx, const Message& m) {
    Peer& p = *mPeers[idx];
    Reader r(m.payload.data(), m.payload.size());
    auto reject = [&](const std::string& why) {
        Writer w(S2C_REJECT);
        w.str(why);
        p.conn->Send(w.done());
        p.conn->Flush();
        Log("Rejected a player: " + why);
    };

    switch (m.type) {
        case C2S_HELLO: {
            uint16_t proto = r.b16();
            std::string build = r.str();
            std::string name = r.str();
            if (!r.ok() || p.hello) {
                return;
            }
            if (proto != kProtocolVersion) {
                reject("Netplay version mismatch. Everyone needs the same SpaghettiKart netplay build.");
                return;
            }
            // First player to say hello defines the build everyone must match.
            for (auto& o : mPeers) {
                if (o->hello && o->build != build) {
                    reject("Game build mismatch (host has " + o->build + ", you have " + build +
                           "). Everyone needs the exact same SpaghettiKart build.");
                    return;
                }
            }
            if (mInSession) {
                reject("A race session is already in progress. Wait for it to end, then join.");
                return;
            }
            if (ActiveCount() >= (size_t) kMaxPlayers) {
                reject("The session is full (4 players).");
                return;
            }
            if (name.empty()) {
                name = "Player";
            }
            p.hello = true;
            p.name = name;
            p.build = build;
            Log(name + " joined");
            size_t pos = 0;
            for (auto& o : mPeers) {
                if (o.get() == &p) {
                    break;
                }
                if (o->hello) {
                    pos++;
                }
            }
            Writer w(S2C_WELCOME);
            w.b8((uint8_t) pos);
            p.conn->Send(w.done());
            BroadcastLobby();
            break;
        }
        case C2S_START_REQ: {
            uint8_t delay = r.b8();
            std::vector<uint8_t> snapshot = r.blob();
            if (!r.ok() || !p.hello || mInSession) {
                return;
            }
            // Only the leader (first player in the lobby) may start.
            Peer* leader = nullptr;
            for (auto& o : mPeers) {
                if (o->hello) {
                    leader = o.get();
                    break;
                }
            }
            if (leader != &p) {
                return;
            }
            if (delay < kMinInputDelay) {
                delay = kMinInputDelay;
            }
            if (delay > kMaxInputDelay) {
                delay = kMaxInputDelay;
            }
            mInSession = true;
            mNextBroadcast = 0;
            mHashes.clear();
            mLastDesyncFrame = UINT32_MAX;
            int c = 0;
            for (auto& o : mPeers) {
                if (o->hello) {
                    o->controller = c++;
                    o->dropped = false;
                    o->nextInputFrame = 0;
                    o->inputs.clear();
                }
            }
            mNumPlayers = c;
            for (auto& o : mPeers) {
                if (o->hello) {
                    Writer w(S2C_START);
                    w.b8((uint8_t) mNumPlayers).b8((uint8_t) o->controller).b8(delay).blob(snapshot);
                    o->conn->Send(w.done());
                }
            }
            Log("Session started with " + std::to_string(mNumPlayers) + " player(s), input delay " +
                std::to_string(delay));
            BroadcastLobby();
            break;
        }
        case C2S_INPUT: {
            uint32_t frame = r.b32();
            Pad pad = r.pad();
            if (!r.ok() || !mInSession || p.controller < 0 || p.dropped) {
                return;
            }
            if (frame != p.nextInputFrame) {
                // TCP keeps order, so this only happens with a buggy client. Ignore it.
                return;
            }
            p.inputs[frame] = pad;
            p.nextInputFrame++;
            TryAdvance();
            break;
        }
        case C2S_HASH: {
            uint32_t frame = r.b32();
            uint32_t hash = r.b32();
            if (!r.ok() || !mInSession || p.controller < 0) {
                return;
            }
            mHashes[frame][p.controller] = hash;
            CheckHashes(frame);
            break;
        }
        case C2S_END_REQ: {
            if (!mInSession || !p.hello) {
                return;
            }
            Peer* leader = nullptr;
            for (auto& o : mPeers) {
                if (o->hello && !o->dropped) {
                    leader = o.get();
                    break;
                }
            }
            if (leader == &p) {
                EndSession("the host ended the session");
            }
            break;
        }
        case C2S_CHAT: {
            std::string text = r.str();
            if (!r.ok() || !p.hello) {
                return;
            }
            Writer w(S2C_CHAT);
            w.b8((uint8_t) (p.controller < 0 ? 0 : p.controller)).str(p.name + ": " + text);
            Broadcast(w.done());
            break;
        }
        default:
            break;
    }
}

void Server::TryAdvance() {
    if (!mInSession) {
        return;
    }
    while (true) {
        std::vector<Pad> pads(mNumPlayers);
        for (auto& o : mPeers) {
            if (o->controller < 0) {
                continue;
            }
            if (o->dropped) {
                continue; // neutral
            }
            auto it = o->inputs.find(mNextBroadcast);
            if (it == o->inputs.end()) {
                return; // still waiting on this player
            }
            pads[o->controller] = it->second;
        }
        Writer w(S2C_INPUTS);
        w.b32(mNextBroadcast).b8((uint8_t) mNumPlayers);
        for (const Pad& pd : pads) {
            w.pad(pd);
        }
        Broadcast(w.done());
        for (auto& o : mPeers) {
            o->inputs.erase(mNextBroadcast);
        }
        mNextBroadcast++;
    }
}

void Server::CheckHashes(uint32_t frame) {
    auto it = mHashes.find(frame);
    if (it == mHashes.end()) {
        return;
    }
    size_t needed = 0;
    for (auto& o : mPeers) {
        if (o->controller >= 0 && !o->dropped) {
            needed++;
        }
    }
    if (it->second.size() < needed) {
        return;
    }
    uint32_t first = it->second.begin()->second;
    bool same = true;
    for (auto& kv : it->second) {
        same &= kv.second == first;
    }
    if (!same && mLastDesyncFrame == UINT32_MAX) {
        mLastDesyncFrame = frame;
        Log("DESYNC detected at frame " + std::to_string(frame));
        Writer w(S2C_DESYNC);
        w.b32(frame);
        Broadcast(w.done());
    }
    // Forget this and anything older
    mHashes.erase(mHashes.begin(), std::next(it));
}

} // namespace Netplay
