// spaghetti-netplay-relay: the online server behind "Host online" / "Join with code" in SpaghettiKart.
//
// Run it once on any always-on machine that has a public address (a cheap VPS, a free cloud VM, or a home PC
// with this one port forwarded). Players never need port forwarding or a VPN: they all connect out to this.
// Build the game with -DNETPLAY_DEFAULT_RELAY=your.server.address so players don't have to type it.
//
//   spaghetti-netplay-relay [port]        (default 25564, or the PORT environment variable)
#include "NetplayProtocol.h"
#include "RoomRelay.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    int port = Netplay::kDefaultPort;
    if (const char* env = getenv("PORT")) {
        port = atoi(env);
    }
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    if (port < 1 || port > 65535) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return 2;
    }
    Netplay::RoomRelay relay;
    std::string err;
    if (!relay.Start((uint16_t) port, err)) {
        fprintf(stderr, "Could not start: %s\n", err.c_str());
        return 1;
    }
    printf("SpaghettiKart online relay listening on TCP port %d. Press Ctrl+C to stop.\n", port);
    fflush(stdout);
    relay.Run();
    return 0;
}
