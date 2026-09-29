// Headless loopback test for the netplay core: server + N clients in threads running a toy deterministic sim.
#include "NetplayClient.h"
#include "NetplayServer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

using namespace Netplay;

static int gFails = 0;
#define CHECK(c, msg)                                                                  \
    do {                                                                               \
        if (!(c)) {                                                                    \
            printf("  FAIL: %s\n", msg);                                               \
            gFails++;                                                                  \
        } else {                                                                       \
            printf("  ok: %s\n", msg);                                                 \
        }                                                                              \
    } while (0)

// Toy "game": karts integrate stick input with an in-game RNG, like MK64's random_u16().
struct Sim {
    uint16_t seed = 0;
    float x[4] = {}, v[4] = {};
    uint32_t items = 0;
    uint16_t rng() {
        seed = (uint16_t) (seed * 0x5D + 0x3039);
        return seed;
    }
    void step(const std::vector<Pad>& pads) {
        for (size_t i = 0; i < pads.size(); i++) {
            v[i] = v[i] * 0.98f + pads[i].stickX * 0.01f;
            if (pads[i].button & 0x8000) {
                v[i] += 0.5f;
            }
            x[i] += v[i];
            if (pads[i].button & 0x2000) {
                items += rng() % 7;
            }
        }
        rng();
    }
    uint32_t hash() const {
        Hasher h;
        h.add(seed);
        h.add(x);
        h.add(v);
        h.add(items);
        return h.h;
    }
};

struct Result {
    bool ok = false;
    uint32_t finalHash = 0;
    uint32_t frames = 0;
    bool sawDesync = false;
    int maxStall = 0;
};

static void RunPlayer(int id, uint16_t port, int frames, int corruptAt, int quitAt, std::atomic<int>* startGate,
                      Result* res) {
    Client c;
    std::string err;
    if (!c.Connect("127.0.0.1", port, "P" + std::to_string(id), "build-1", err)) {
        printf("  player %d connect failed: %s\n", id, err.c_str());
        return;
    }
    // Wait for the lobby to fill, then the leader starts.
    auto t0 = std::chrono::steady_clock::now();
    while (c.GetState() == Client::State::Lobby) {
        c.Poll();
        if (c.IsLeader() && (int) c.LobbyNames().size() == startGate->load()) {
            Sim host;
            host.seed = 1234;
            std::vector<uint8_t> snap(sizeof(host));
            memcpy(snap.data(), &host, sizeof(host));
            c.RequestStart(3, snap);
            startGate->store(-1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10)) {
            printf("  player %d: lobby timeout\n", id);
            return;
        }
    }
    if (c.GetState() != Client::State::StartPending) {
        return;
    }
    Sim sim;
    memcpy(&sim, c.Snapshot().data(), sizeof(sim)); // apply host snapshot
    c.BeginSession();

    std::mt19937 rnd(id * 7919 + 1); // each player presses different buttons
    std::vector<Pad> pads;
    for (int f = 0; f < frames; f++) {
        if (f == quitAt) {
            c.Disconnect();
            res->ok = true;
            res->frames = f;
            return;
        }
        Pad local;
        local.stickX = (int8_t) (rnd() % 160 - 80);
        local.button = (uint16_t) ((rnd() % 3 == 0 ? 0x8000 : 0) | (rnd() % 5 == 0 ? 0x2000 : 0));
        Client::FrameResult fr;
        while ((fr = c.ExchangeFrame(local, pads, 50)) == Client::FrameResult::Waiting) {
        }
        if (fr == Client::FrameResult::Lost) {
            printf("  player %d lost connection at frame %d\n", id, f);
            return;
        }
        if ((uint32_t) f % kHashInterval == 0) {
            c.SendHash(f, sim.hash());
        }
        sim.step(pads);
        if (f == corruptAt) {
            sim.x[0] += 0.001f; // simulate a nondeterminism bug on this machine
        }
        uint32_t df;
        if (c.TakeDesync(df)) {
            res->sawDesync = true;
        }
        if (c.StallMsLastSecond() > res->maxStall) {
            res->maxStall = c.StallMsLastSecond();
        }
        // Simulate ~30 fps with jitter, faster than real time to keep the test short
        std::this_thread::sleep_for(std::chrono::microseconds(500 + rnd() % 1500));
    }
    // Drain for late desync notice
    auto t1 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(300)) {
        c.Poll();
        uint32_t df;
        if (c.TakeDesync(df)) {
            res->sawDesync = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    res->ok = true;
    res->finalHash = sim.hash();
    res->frames = frames;
}

static void Scenario(const char* name, int players, int frames, int corruptPlayer, int quitPlayer, uint16_t port,
                     std::vector<Result>& out) {
    printf("\n== %s\n", name);
    Server s;
    std::string err;
    if (!s.Start(port, err)) {
        printf("  server failed: %s\n", err.c_str());
        gFails++;
        return;
    }
    std::atomic<int> gate(players);
    out.assign(players, Result{});
    std::vector<std::thread> th;
    for (int i = 0; i < players; i++) {
        th.emplace_back(RunPlayer, i, port, frames, i == corruptPlayer ? 150 : -1, i == quitPlayer ? 200 : -1, &gate,
                        &out[i]);
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // join in order: player 0 = leader
    }
    for (auto& t : th) {
        t.join();
    }
    s.Stop();
}

int main() {
    NetInit();
    std::vector<Result> r;

    Scenario("3 players, 1200 frames, no bugs -> identical games", 3, 1200, -1, -1, 25601, r);
    CHECK(r[0].ok && r[1].ok && r[2].ok, "all players finished");
    CHECK(r[0].finalHash == r[1].finalHash && r[1].finalHash == r[2].finalHash, "final game state identical");
    CHECK(!r[0].sawDesync && !r[1].sawDesync && !r[2].sawDesync, "no false desync alarms");
    printf("  final hash %08x, worst stall %d ms/s\n", r[0].finalHash, r[0].maxStall);

    Scenario("4 players, player 2's game drifts at frame 150 -> desync reported", 4, 400, 2, -1, 25602, r);
    CHECK(r[0].sawDesync && r[1].sawDesync && r[2].sawDesync && r[3].sawDesync, "every player told about desync");
    CHECK(r[0].finalHash == r[1].finalHash && r[0].finalHash != r[2].finalHash, "only the buggy game diverged");

    Scenario("3 players, player 1 quits at frame 200 -> race continues", 3, 600, -1, 1, 25603, r);
    CHECK(r[0].ok && r[2].ok && r[0].frames == 600 && r[2].frames == 600, "remaining players finished the race");
    CHECK(r[0].finalHash == r[2].finalHash, "remaining games still identical");

    // Build mismatch is rejected
    printf("\n== wrong build is refused\n");
    {
        Server s;
        std::string err;
        s.Start(25604, err);
        Client a, b;
        a.Connect("127.0.0.1", 25604, "A", "build-1", err);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        b.Connect("127.0.0.1", 25604, "B", "build-2", err);
        for (int i = 0; i < 100 && b.GetState() != Client::State::Disconnected; i++) {
            a.Poll();
            b.Poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(b.GetState() == Client::State::Disconnected, "mismatched build disconnected");
        auto log = b.TakeLog();
        bool mentioned = false;
        for (auto& l : log) {
            mentioned |= l.find("build mismatch") != std::string::npos;
        }
        CHECK(mentioned, "player told why");
        s.Stop();
    }

    // Host ends a session (e.g. after a desync) and starts a fresh one
    printf("\n== host ends the session, then starts a new one\n");
    {
        Server s;
        std::string err;
        s.Start(25605, err);
        Client a, b;
        a.Connect("127.0.0.1", 25605, "A", "build-1", err);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        b.Connect("127.0.0.1", 25605, "B", "build-1", err);
        auto pollBoth = [&](int ms) {
            for (int i = 0; i < ms / 5; i++) {
                a.Poll();
                b.Poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        };
        // Both players on one thread: keep calling each until both have their frame.
        auto runFrames = [&](int n) {
            std::vector<Pad> pa, pb;
            Sim sa, sb;
            for (int f = 0; f < n; f++) {
                Pad la, lb;
                la.stickX = (int8_t) (f % 50);
                lb.stickX = (int8_t) -(f % 30);
                bool ra = false, rb = false;
                for (int tries = 0; (!ra || !rb) && tries < 2000; tries++) {
                    if (!ra) {
                        ra = a.ExchangeFrame(la, pa, 2) == Client::FrameResult::Ready;
                    }
                    if (!rb) {
                        rb = b.ExchangeFrame(lb, pb, 2) == Client::FrameResult::Ready;
                    }
                }
                if (!ra || !rb) {
                    return false;
                }
                sa.step(pa);
                sb.step(pb);
            }
            return sa.hash() == sb.hash();
        };
        pollBoth(100);
        a.RequestStart(2, {});
        pollBoth(100);
        a.BeginSession();
        b.BeginSession();
        bool first = runFrames(100);
        a.RequestEnd();
        pollBoth(150);
        std::string why;
        bool endedA = a.TakeSessionEnded(why), endedB = b.TakeSessionEnded(why);
        CHECK(first, "first session stayed identical");
        CHECK(endedA && endedB && a.GetState() == Client::State::Lobby && b.GetState() == Client::State::Lobby,
              "both players back in the lobby");
        a.RequestStart(2, {});
        pollBoth(100);
        bool started = a.GetState() == Client::State::StartPending && b.GetState() == Client::State::StartPending;
        a.BeginSession();
        b.BeginSession();
        CHECK(started && runFrames(100), "second session starts and stays identical");
        s.Stop();
    }

    printf("\n%s (%d failure%s)\n", gFails ? "FAILED" : "ALL PASSED", gFails, gFails == 1 ? "" : "s");
    return gFails ? 1 : 0;
}
