#pragma once
// Netplay client: lobby handling and the per-frame lockstep input exchange.
// Game-agnostic: the game glue (NetplayGame.cpp) feeds it the local controller and applies what comes back.
#include "NetplayProtocol.h"
#include "NetplaySocket.h"

#include <map>
#include <string>
#include <vector>

namespace Netplay {

class Client {
  public:
    enum class State { Disconnected, Lobby, StartPending, InSession };
    enum class FrameResult { Ready, Waiting, Lost };

    bool Connect(const std::string& host, uint16_t port, const std::string& name, const std::string& build,
                 std::string& err);
    // Online play through a relay. Empty roomCode = create a new room. On success RoomCode() holds the code
    // to give to friends.
    bool ConnectRoom(const std::string& relayHost, uint16_t relayPort, const std::string& roomCode,
                     const std::string& name, const std::string& build, std::string& err);
    const std::string& RoomCode() const {
        return mRoomCode;
    }
    void Disconnect();

    // Call every frame (lobby or session). Processes network messages.
    void Poll();

    // Lobby
    bool IsLeader() const {
        return mLobbyIndex == 0;
    }
    const std::vector<std::string>& LobbyNames() const {
        return mLobby;
    }
    void RequestStart(int inputDelay, const std::vector<uint8_t>& snapshot);
    void SendChat(const std::string& text);
    void RequestEnd();

    // Session start handshake: when State() == StartPending, the game applies Snapshot(), then calls BeginSession().
    const std::vector<uint8_t>& Snapshot() const {
        return mSnapshot;
    }
    void BeginSession();

    // Lockstep. Call exactly once per game frame, before the game reads controllers.
    // Sends `local` for frame (current + inputDelay) the first time it's called for a frame, then waits up to
    // `waitMs` for everyone's input for the current frame. On Ready, `out` holds NumPlayers() pads and the
    // frame counter advances. On Waiting, call again next time (the local input is not re-sent).
    FrameResult ExchangeFrame(const Pad& local, std::vector<Pad>& out, int waitMs);
    void SendHash(uint32_t frame, uint32_t hash);

    State GetState() const {
        return mState;
    }
    int NumPlayers() const {
        return mNumPlayers;
    }
    int MyController() const {
        return mMyController;
    }
    int InputDelay() const {
        return mInputDelay;
    }
    uint32_t Frame() const {
        return mFrame;
    }
    // Milliseconds this client spent waiting on the network during the last second (lag indicator).
    int StallMsLastSecond() const {
        return mStallReport;
    }

    // Events for the UI. Drained by the caller.
    std::vector<std::string> TakeLog();
    bool TakeDesync(uint32_t& frame);
    bool TakeSessionEnded(std::string& why);

  private:
    void Handle(const Message& m);
    bool SendHello(const std::string& name, const std::string& build, std::string& err);
    void Log(const std::string& s);

    Connection mConn;
    std::string mRoomCode;
    State mState = State::Disconnected;
    int mLobbyIndex = -1;
    std::vector<std::string> mLobby;
    std::vector<uint8_t> mSnapshot;
    int mNumPlayers = 0;
    int mMyController = 0;
    int mInputDelay = 2;
    uint32_t mFrame = 0;
    uint32_t mSentUpTo = 0; // next frame number we will send input for
    std::map<uint32_t, std::vector<Pad>> mInputs;

    std::vector<std::string> mLog;
    bool mDesync = false;
    uint32_t mDesyncFrame = 0;
    bool mEnded = false;
    std::string mEndReason;

    uint64_t mStallAccumUs = 0;
    uint64_t mStallWindowStartUs = 0;
    int mStallReport = 0;
};

} // namespace Netplay
