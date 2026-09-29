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
};

std::string BuildId();
bool Host(uint16_t port, const std::string& name, std::string& err);
bool Join(const std::string& host, uint16_t port, const std::string& name, std::string& err);
void Leave();
void StartSession(int inputDelay);
void EndSession(); // leader only: everyone returns to the lobby
void SendChat(const std::string& text);
Status GetStatus();

} // namespace Netplay
