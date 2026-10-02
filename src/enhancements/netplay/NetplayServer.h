#pragma once
// Netplay host/relay. Has no dependency on the game, so the same code is used both inside SpaghettiKart
// ("Host on this PC") and in the standalone spaghetti-netplay-relay program.
//
// Two ways to drive it:
//  - Start(port): listens on a TCP port and runs on its own thread (hosting from inside the game).
//  - Adopt()/Step(): no listener, no thread. The owner hands it connections and calls Step() regularly.
//    The online relay uses this to run one Server per room code.
#include "NetplayProtocol.h"
#include "NetplaySocket.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Netplay {

class Server {
  public:
    ~Server();
    bool Start(uint16_t port, std::string& err);
    void Stop();
    bool IsRunning() const {
        return mRunning;
    }
    uint16_t Port() const {
        return mPort;
    }

    // Manual mode (see top of file). Not thread-safe with Start(); use one or the other.
    void Adopt(std::unique_ptr<Connection> conn);
    void Step();                                            // process everything that has arrived
    void CollectSockets(std::vector<NetSocketHandle>& out); // sockets worth waiting on
    size_t PeerCount() const {
        return mPeers.size();
    }
    void SetLogTag(const std::string& tag) {
        mLogTag = tag;
    }

    struct Status {
        bool inSession = false;
        std::vector<std::string> players; // lobby order
        std::vector<std::string> log;
    };
    Status GetStatus();

  private:
    struct Peer {
        std::unique_ptr<Connection> conn;
        std::string name;
        std::string build;
        bool hello = false;
        int controller = -1;  // assigned at session start
        bool dropped = false; // left mid-session: fed neutral input
        uint32_t nextInputFrame = 0;
        std::map<uint32_t, Pad> inputs;
    };

    void Run();
    void HandleMessage(size_t peerIdx, const Message& m);
    bool RemovePeer(size_t peerIdx, const std::string& why);
    void BroadcastLobby();
    void Broadcast(const std::vector<uint8_t>& framed);
    void TryAdvance();
    void CheckHashes(uint32_t frame);
    void EndSession(const std::string& why);
    void Log(const std::string& s);
    size_t ActiveCount() const;

    Listener mListener;
    std::thread mThread;
    std::atomic<bool> mRunning{ false };
    std::atomic<bool> mStopRequested{ false };
    uint16_t mPort = 0;
    std::string mLogTag = "netplay-host";

    // Owned by the server thread
    std::vector<std::unique_ptr<Peer>> mPeers; // index 0 = leader (may start the session)
    bool mInSession = false;
    int mNumPlayers = 0;
    uint32_t mNextBroadcast = 0;
    std::map<uint32_t, std::map<int, uint32_t>> mHashes; // frame -> controller -> hash
    uint32_t mLastDesyncFrame = UINT32_MAX;

    std::mutex mStatusMutex;
    Status mStatus;
};

} // namespace Netplay
