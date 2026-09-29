#include "NetplayClient.h"

#include <chrono>
#include <cstdio>

namespace Netplay {

static uint64_t NowUs() {
    return (uint64_t) std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Client::Log(const std::string& s) {
    mLog.push_back(s);
    fprintf(stderr, "[netplay] %s\n", s.c_str());
}

std::vector<std::string> Client::TakeLog() {
    std::vector<std::string> out;
    out.swap(mLog);
    return out;
}

bool Client::TakeDesync(uint32_t& frame) {
    if (!mDesync) {
        return false;
    }
    mDesync = false;
    frame = mDesyncFrame;
    return true;
}

bool Client::TakeSessionEnded(std::string& why) {
    if (!mEnded) {
        return false;
    }
    mEnded = false;
    why = mEndReason;
    return true;
}

bool Client::SendHello(const std::string& name, const std::string& build, std::string& err) {
    Writer w(C2S_HELLO);
    w.b16(kProtocolVersion).str(build).str(name);
    if (!mConn.Send(w.done())) {
        err = "Connection dropped right after connecting.";
        mConn.Close();
        return false;
    }
    mState = State::Lobby;
    mLobbyIndex = -1;
    mLobby.clear();
    return true;
}

bool Client::Connect(const std::string& host, uint16_t port, const std::string& name, const std::string& build,
                     std::string& err) {
    Disconnect();
    if (!mConn.Connect(host, port, 5000, err)) {
        err += " Is the host running, and is the port forwarded or are you on the same VPN?";
        return false;
    }
    if (!SendHello(name, build, err)) {
        return false;
    }
    Log("Connected to " + host + ":" + std::to_string(port));
    return true;
}

bool Client::ConnectRoom(const std::string& relayHost, uint16_t relayPort, const std::string& roomCode,
                         const std::string& name, const std::string& build, std::string& err) {
    Disconnect();
    if (relayHost.empty()) {
        err = "No online server is set up in this build. Set one under Advanced, or host on this PC.";
        return false;
    }
    if (!mConn.Connect(relayHost, relayPort, 6000, err)) {
        err = "Could not reach the online server (" + relayHost + "). Check your internet connection, or the "
              "server may be down.";
        return false;
    }
    std::string code = NormalizeRoomCode(roomCode);
    if (code.empty()) {
        Writer w(C2S_ROOM_CREATE);
        w.b16(kProtocolVersion);
        mConn.Send(w.done());
    } else {
        Writer w(C2S_ROOM_JOIN);
        w.b16(kProtocolVersion).str(code);
        mConn.Send(w.done());
    }
    // Wait for the relay to put us in a room.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (true) {
        bool alive = mConn.Pump();
        Message m;
        if (mConn.PopMessage(m)) {
            Reader r(m.payload.data(), m.payload.size());
            if (m.type == S2C_ROOM) {
                mRoomCode = r.str();
                break;
            }
            if (m.type == S2C_REJECT) {
                err = r.str();
                mConn.Close();
                return false;
            }
            continue; // ignore anything else before the room answer
        }
        if (!alive) {
            err = "The online server closed the connection.";
            mConn.Close();
            return false;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            err = "The online server did not answer.";
            mConn.Close();
            return false;
        }
        WaitReadable({ mConn.Handle() }, 50);
    }
    if (!SendHello(name, build, err)) {
        return false;
    }
    Log(code.empty() ? "Room " + mRoomCode + " created. Give this code to your friends."
                     : "Joined room " + mRoomCode + ".");
    return true;
}

void Client::Disconnect() {
    mConn.Close();
    mRoomCode.clear();
    bool wasInSession = mState == State::InSession || mState == State::StartPending;
    mState = State::Disconnected;
    mInputs.clear();
    mLobby.clear();
    mLobbyIndex = -1;
    if (wasInSession) {
        mEnded = true;
        mEndReason = "Disconnected";
    }
}

void Client::Poll() {
    if (mState == State::Disconnected) {
        return;
    }
    bool alive = mConn.Pump();
    Message m;
    while (mConn.PopMessage(m)) {
        Handle(m);
        if (mState == State::Disconnected) {
            return;
        }
    }
    if (!alive) {
        Log("Lost connection to the host.");
        Disconnect();
    }
}

void Client::Handle(const Message& m) {
    Reader r(m.payload.data(), m.payload.size());
    switch (m.type) {
        case S2C_WELCOME:
            mLobbyIndex = r.b8();
            break;
        case S2C_REJECT: {
            std::string why = r.str();
            Log("Host refused the connection: " + why);
            Disconnect();
            break;
        }
        case S2C_LOBBY: {
            uint8_t n = r.b8();
            std::vector<std::string> names;
            for (int i = 0; i < n && r.ok(); i++) {
                r.b8();
                names.push_back(r.str());
            }
            if (r.ok()) {
                mLobby = names;
            }
            break;
        }
        case S2C_START: {
            if (mState != State::Lobby) {
                break;
            }
            int n = r.b8();
            int me = r.b8();
            int delay = r.b8();
            std::vector<uint8_t> snap = r.blob();
            if (!r.ok() || n < 1 || n > kMaxPlayers || me >= n) {
                break;
            }
            mNumPlayers = n;
            mMyController = me;
            mInputDelay = delay;
            mSnapshot = std::move(snap);
            mState = State::StartPending;
            Log("Race session starting: " + std::to_string(n) + " player(s), you are player " +
                std::to_string(me + 1));
            break;
        }
        case S2C_INPUTS: {
            uint32_t frame = r.b32();
            int n = r.b8();
            std::vector<Pad> pads;
            for (int i = 0; i < n; i++) {
                pads.push_back(r.pad());
            }
            if (r.ok() && n == mNumPlayers && frame >= mFrame) {
                mInputs[frame] = std::move(pads);
            }
            break;
        }
        case S2C_DESYNC:
            mDesyncFrame = r.b32();
            mDesync = true;
            Log("Desync detected at frame " + std::to_string(mDesyncFrame) +
                ": the games are no longer identical. Return to the menu and start a new session.");
            break;
        case S2C_PLAYER_LEFT: {
            int slot = r.b8();
            std::string name = r.str();
            Log(name + " (player " + std::to_string(slot + 1) + ") left. Their kart is now idle.");
            break;
        }
        case S2C_CHAT:
            r.b8();
            Log(r.str());
            break;
        case S2C_SESSION_END: {
            std::string why = r.str();
            if (mState == State::InSession || mState == State::StartPending) {
                mEnded = true;
                mEndReason = why;
            }
            mState = State::Lobby;
            mInputs.clear();
            break;
        }
        default:
            break;
    }
}

void Client::RequestStart(int inputDelay, const std::vector<uint8_t>& snapshot) {
    if (mState != State::Lobby) {
        return;
    }
    Writer w(C2S_START_REQ);
    w.b8((uint8_t) inputDelay).blob(snapshot);
    mConn.Send(w.done());
}

void Client::RequestEnd() {
    if (mState != State::InSession) {
        return;
    }
    Writer w(C2S_END_REQ);
    mConn.Send(w.done());
}

void Client::SendChat(const std::string& text) {
    if (mState == State::Disconnected) {
        return;
    }
    Writer w(C2S_CHAT);
    w.str(text);
    mConn.Send(w.done());
}

void Client::BeginSession() {
    if (mState != State::StartPending) {
        return;
    }
    mState = State::InSession;
    mFrame = 0;
    mSentUpTo = 0;
    mInputs.clear();
    mStallAccumUs = 0;
    mStallWindowStartUs = NowUs();
    // Nobody can have real input for the first `inputDelay` frames, so everyone agrees they are neutral.
    Pad neutral;
    for (int f = 0; f < mInputDelay; f++) {
        Writer w(C2S_INPUT);
        w.b32(mSentUpTo++).pad(neutral);
        mConn.Send(w.done());
    }
}

Client::FrameResult Client::ExchangeFrame(const Pad& local, std::vector<Pad>& out, int waitMs) {
    if (mState != State::InSession) {
        return FrameResult::Lost;
    }
    // Send this frame's local input, scheduled inputDelay frames ahead (only once per frame).
    if (mSentUpTo == mFrame + (uint32_t) mInputDelay) {
        Writer w(C2S_INPUT);
        w.b32(mSentUpTo++).pad(local);
        if (!mConn.Send(w.done())) {
            Log("Lost connection to the host.");
            Disconnect();
            return FrameResult::Lost;
        }
    }

    uint64_t start = NowUs();
    uint64_t deadline = start + (uint64_t) waitMs * 1000;
    while (true) {
        Poll();
        if (mState != State::InSession) {
            return FrameResult::Lost;
        }
        auto it = mInputs.find(mFrame);
        if (it != mInputs.end()) {
            out = std::move(it->second);
            mInputs.erase(it);
            mFrame++;
            break;
        }
        uint64_t now = NowUs();
        if (now >= deadline) {
            mStallAccumUs += now - start;
            return FrameResult::Waiting;
        }
        int left = (int) ((deadline - now) / 1000);
        WaitReadable({ mConn.Handle() }, left < 1 ? 1 : left);
    }

    uint64_t now = NowUs();
    mStallAccumUs += now - start;
    if (now - mStallWindowStartUs >= 1000000) {
        mStallReport = (int) (mStallAccumUs / 1000);
        mStallAccumUs = 0;
        mStallWindowStartUs = now;
    }
    return FrameResult::Ready;
}

void Client::SendHash(uint32_t frame, uint32_t hash) {
    if (mState != State::InSession) {
        return;
    }
    Writer w(C2S_HASH);
    w.b32(frame).b32(hash);
    mConn.Send(w.done());
}

} // namespace Netplay
