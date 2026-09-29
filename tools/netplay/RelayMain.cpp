// spaghetti-netplay-relay: a dedicated host for SpaghettiKart online sessions.
// Run it on any machine that friends can reach (a PC with the port forwarded, or a cheap VPS), then everyone
// uses "Join" in the game. The first player to join is the session leader who picks settings and starts.
//
//   spaghetti-netplay-relay [port]
#include "NetplayProtocol.h"
#include "NetplayServer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    int port = Netplay::kDefaultPort;
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    if (port < 1 || port > 65535) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return 2;
    }
    Netplay::NetInit();
    Netplay::Server server;
    std::string err;
    if (!server.Start((uint16_t) port, err)) {
        fprintf(stderr, "Could not start: %s\n", err.c_str());
        return 1;
    }
    printf("SpaghettiKart netplay relay listening on TCP port %d. Press Ctrl+C to stop.\n", port);
    fflush(stdout);
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
}
