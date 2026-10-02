#pragma once
// Online relay for SpaghettiKart netplay: one public server that many groups of friends share.
//
// Everybody (including the person who "hosts") connects OUT to the relay, so nobody needs port forwarding or a
// VPN. The first message on a connection picks a room (see C2S_ROOM_CREATE / C2S_ROOM_JOIN in
// NetplayProtocol.h). Each room is an ordinary lockstep Netplay::Server driven in manual mode.
//
// Connections whose first message is C2S_HELLO (older builds using plain "Join" with an address) all go into
// one shared "direct" room, which is how the relay worked before room codes existed.
#include "NetplayProtocol.h"
#include "NetplayServer.h"
#include "NetplaySocket.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace Netplay {

class RoomRelay {
  public:
    struct Limits {
        size_t maxConnections = 900; // select() on Linux can't watch fds >= 1024
        size_t maxRooms = 300;
        int pendingTimeoutMs = 10000; // time allowed to send the room request
    };

    ~RoomRelay();
    bool Start(uint16_t port, std::string& err);
    void SetLimits(const Limits& l) {
        mLimits = l;
    }
    // Runs until Stop() (or forever). RunOnce is one iteration, waiting at most waitMs for network activity.
    void Run();
    void RunOnce(int waitMs);
    // Runs Run() on a background thread (used by tests).
    void StartThread();
    void Stop();

    size_t RoomCount() const {
        return mRoomCount;
    }

  private:
    struct Pending {
        std::unique_ptr<Connection> conn;
        std::chrono::steady_clock::time_point since;
    };
    struct Room {
        std::unique_ptr<Server> server;
    };

    void HandlePending(Pending& p, bool& remove);
    std::string NewCode();
    size_t TotalConnections() const;
    void Reject(Connection& c, const std::string& why);

    Listener mListener;
    Limits mLimits;
    std::vector<Pending> mPending;
    std::map<std::string, Room> mRooms; // "" = shared direct room for plain HELLO connections
    std::mt19937 mRng{ std::random_device{}() };
    std::atomic<bool> mStop{ false };
    std::atomic<size_t> mRoomCount{ 0 };
    std::thread mThread;
    std::chrono::steady_clock::time_point mLastStats = std::chrono::steady_clock::now();
};

} // namespace Netplay
