#pragma once
// C++ API used by the Netplay window.
#include <cstdint>
#include <string>
#include <vector>

namespace Netplay {

struct Status {
    std::string connection;
    bool connected = false;
    bool hosting = false;
    bool isLeader = false;
    bool inSession = false;
    std::vector<std::string> lobby;
    int myPlayer = 0;
    int numPlayers = 0;
    int inputDelay = 0;
    uint32_t frame = 0;
    int stallMs = 0;
    std::vector<std::string> log;
    std::string banner;
    bool desynced = false;
    std::string roomCode; // set when playing through the online server
};

std::string BuildId();
// Online play through the relay server: no port forwarding or VPN for anyone.
std::string RelayAddress();     // the player's override (Advanced) or the address built into this build
bool HostOnline(const std::string& name, std::string& err);                            // creates a room code
bool JoinRoom(const std::string& code, const std::string& name, std::string& err);
// Direct connections (Advanced): the host needs a forwarded port or a shared VPN.
bool Host(uint16_t port, const std::string& name, std::string& err);
bool Join(const std::string& host, uint16_t port, const std::string& name, std::string& err);
void Leave();
void StartSession(int inputDelay);
void EndSession(); // leader only: everyone returns to the lobby
void SendChat(const std::string& text);
Status GetStatus();

} // namespace Netplay
