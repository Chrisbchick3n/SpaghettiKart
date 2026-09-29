// Game glue for lockstep netplay: builds/applies the session snapshot, runs the per-frame input exchange,
// and computes the desync-check hash. See NetplayProtocol.h for how the lockstep model works.
#include "NetplayGame.h"
#include "NetplayClient.h"
#include "NetplayServer.h"
#include "netplay.h"

#include <libultraship.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>

#include "port/Game.h"

extern "C" {
#include "main.h"
#include "menus.h"
#include "buffers.h"
#include "code_800029B0.h"
#include "save_data.h"
}

#ifndef NETPLAY_BUILD_ID
#define NETPLAY_BUILD_ID "SpaghettiKart-unknown"
#endif

namespace Netplay {

namespace {

// Settings that change how the game plays. The host's values are copied to everyone for the session
// (including "not set", so each setting's built-in default applies), then restored afterwards.
struct SyncedCVar {
    const char* name;
    bool isFloat;
};
const SyncedCVar kSyncedCVars[] = {
    { "gNoWallColision", false },      { "gHarderCPU", false },           { "gShellsShootStraight", false },
    { "gMultiplayerNoFeatureCuts", false }, { "gEnableCustomCC", false }, { "gCustomCC", true },
    { "gSkipIntro", false },           { "gUniqueCharacterSelections", false },
    { "gNumTrucks", false },           { "gNumTankerTrucks", false },     { "gNumCars", false },
    { "gNumBuses", false },            { "gNumCarriages", false },        { "gNumTrains", false },
    { "gHasTender", false },           { "gGoFish", false },              { "gEnableMoonJump", false },
    { "gDisableRubberbanding", false }, { "gDisableItemboxes", false },   { "gAllThwompsAreMarty", false },
    { "gAllBombKartsChase", false },   { "gEnableDebugMode", false },     { "gNoCulling", false },
    { "gDisableLod", false },          { "gControllerPakScreen", false }, { "gMinHeight", true },
    { "gLookBehind", false },
};
constexpr size_t kNumSyncedCVars = sizeof(kSyncedCVars) / sizeof(kSyncedCVars[0]);

// (libultraship declares CVarExists but doesn't implement it.)
bool SettingIsSet(const char* name) {
    return CVarGet(name) != nullptr;
}

// Raw game memory copied from the host at session start. Order and sizes must match on every build
// (they do, because the build id must match).
struct RawBlock {
    void* ptr;
    size_t size;
};
std::vector<RawBlock> RawBlocks() {
    return {
        { &gSaveData, sizeof(gSaveData) },
        { pAppNmiBuffer, 32 }, // VS/battle win counters shown on the results screens
        { &gRandomSeed16, sizeof(gRandomSeed16) },
        { &gModeSelection, sizeof(gModeSelection) },
        { &gCCSelection, sizeof(gCCSelection) },
        { &gPlayerCountSelection1, sizeof(gPlayerCountSelection1) },
        { &gScreenModeSelection, sizeof(gScreenModeSelection) },
        { &gActiveScreenMode, sizeof(gActiveScreenMode) },
        { &gGlobalTimer, sizeof(gGlobalTimer) },
        { &gCourseTimer, sizeof(gCourseTimer) },
        { &gVBlankTimer, sizeof(gVBlankTimer) },
        { &gDemoMode, sizeof(gDemoMode) },
        { gCharacterSelections, 4 },
        { gCharacterGridSelections, 4 },
        { gCharacterGridIsSelected, 4 * sizeof(bool) },
        { &gSubMenuSelection, sizeof(gSubMenuSelection) },
        { &gMainMenuSelection, sizeof(gMainMenuSelection) },
        { &gPlayerSelectMenuSelection, sizeof(gPlayerSelectMenuSelection) },
        { &gDebugMenuSelection, sizeof(gDebugMenuSelection) },
        { &gScreenModeListIndex, sizeof(gScreenModeListIndex) },
        { &gSoundMode, sizeof(gSoundMode) },
        { &gPlayerCount, sizeof(gPlayerCount) },
        { &gCupSelection, sizeof(gCupSelection) },
        { &gCourseIndexInCup, sizeof(gCourseIndexInCup) },
        { &gNextDemoId, sizeof(gNextDemoId) },
        { &gTimeTrialDataCourseIndex, sizeof(gTimeTrialDataCourseIndex) },
        { &gDemoUseController, sizeof(gDemoUseController) },
        { &gMenuTimingCounter, sizeof(gMenuTimingCounter) },
        { &gMenuDelayTimer, sizeof(gMenuDelayTimer) },
        { gControllerStatuses, 4 * sizeof(OSContStatus) },
    };
}

constexpr uint32_t kSnapshotMagic = 0x4E50534B; // "KSPN"

struct SavedCVar {
    bool existed;
    int32_t i;
    float f;
};

struct State {
    Client client;
    std::unique_ptr<Server> server;
    std::string playerName = "Player";
    int inputDelay = 3;

    bool resetPending = false;     // snapshot waiting for the soft reset to apply it
    bool sessionActive = false;
    std::vector<SavedCVar> savedCVars;
    std::vector<uint8_t> savedRaw; // this player's own save data etc., restored after the session
    int savedTickLogic = 2;
    u8 savedControllerBits = 0;

    std::vector<std::string> log;
    std::string banner;           // shown prominently in the netplay window
    bool desynced = false;
    int stallMs = 0;
} sState;

void AddLog(const std::string& s) {
    sState.log.push_back(s);
    if (sState.log.size() > 100) {
        sState.log.erase(sState.log.begin());
    }
    SPDLOG_INFO("[Netplay] {}", s);
}

// ---------------------------------------------------------------------------
std::vector<uint8_t> BuildSnapshot() {
    Writer w(0); // reuse the serializer; the 5-byte header is stripped below
    w.b32(kSnapshotMagic);
    w.b32((uint32_t) kNumSyncedCVars);
    for (const SyncedCVar& c : kSyncedCVars) {
        bool exists = SettingIsSet(c.name);
        uint32_t bits = 0;
        if (c.isFloat) {
            float f = CVarGetFloat(c.name, 0.0f);
            memcpy(&bits, &f, 4);
        } else {
            bits = (uint32_t) CVarGetInteger(c.name, 0);
        }
        w.b8(exists ? 1 : 0).b32(bits);
    }
    for (const RawBlock& b : RawBlocks()) {
        w.b32((uint32_t) b.size).bytes(b.ptr, b.size);
    }
    std::vector<uint8_t> v = w.done();
    return std::vector<uint8_t>(v.begin() + 5, v.end());
}

void SaveLocalState() {
    sState.savedCVars.clear();
    for (const SyncedCVar& c : kSyncedCVars) {
        SavedCVar s;
        s.existed = SettingIsSet(c.name);
        s.i = CVarGetInteger(c.name, 0);
        s.f = CVarGetFloat(c.name, 0.0f);
        sState.savedCVars.push_back(s);
    }
    sState.savedRaw.assign((uint8_t*) &gSaveData, (uint8_t*) &gSaveData + sizeof(gSaveData));
    sState.savedTickLogic = gTickLogic;
    sState.savedControllerBits = gControllerBits;
}

void RestoreLocalState() {
    for (size_t i = 0; i < kNumSyncedCVars && i < sState.savedCVars.size(); i++) {
        const SyncedCVar& c = kSyncedCVars[i];
        const SavedCVar& s = sState.savedCVars[i];
        if (!s.existed) {
            CVarClear(c.name);
        } else if (c.isFloat) {
            CVarSetFloat(c.name, s.f);
        } else {
            CVarSetInteger(c.name, s.i);
        }
    }
    if (sState.savedRaw.size() == sizeof(gSaveData)) {
        memcpy(&gSaveData, sState.savedRaw.data(), sizeof(gSaveData));
    }
    gTickLogic = sState.savedTickLogic;
    gControllerBits = sState.savedControllerBits;
}

// Applies settings. Game memory is applied separately in ApplySnapshotMemory(), after the soft reset.
bool ApplySnapshotSettings(const std::vector<uint8_t>& snap) {
    Reader r(snap.data(), snap.size());
    if (r.b32() != kSnapshotMagic || r.b32() != kNumSyncedCVars) {
        return false;
    }
    for (const SyncedCVar& c : kSyncedCVars) {
        bool exists = r.b8() != 0;
        uint32_t bits = r.b32();
        if (!exists) {
            CVarClear(c.name);
        } else if (c.isFloat) {
            float f;
            memcpy(&f, &bits, 4);
            CVarSetFloat(c.name, f);
        } else {
            CVarSetInteger(c.name, (int32_t) bits);
        }
    }
    return r.ok();
}

bool ApplySnapshotMemory(const std::vector<uint8_t>& snap) {
    Reader r(snap.data(), snap.size());
    r.b32();
    r.b32();
    for (size_t i = 0; i < kNumSyncedCVars; i++) {
        r.b8();
        r.b32();
    }
    for (const RawBlock& b : RawBlocks()) {
        uint32_t n = r.b32();
        if (!r.ok() || n != b.size) {
            return false;
        }
        // Read into a temp first so a truncated snapshot can't half-apply
        std::vector<uint8_t> tmp(n);
        for (uint32_t k = 0; k < n; k++) {
            tmp[k] = r.b8();
        }
        if (!r.ok()) {
            return false;
        }
        memcpy(b.ptr, tmp.data(), n);
    }
    return true;
}

// Things that only affect this machine but would change the simulation. Forced for the whole session.
void ForceLockstepSafeLocalSettings() {
    gTickLogic = 2;          // the debug "game speed" tool
    CVarSetInteger("gFreecam", 0); // freecam steals controller 1's input
    gControllerBits = 0x0F;  // every machine sees 4 controllers, so all player counts are selectable
    sIsController1Unplugged = 0; // otherwise this machine would ignore every synced input
}

uint32_t StateHash() {
    Hasher h;
    h.add(gRandomSeed16);
    h.add(gGamestate);
    h.add(gMenuSelection);
    h.add(gModeSelection);
    h.add(gCCSelection);
    h.add(gPlayerCountSelection1);
    h.add(gRaceState);
    h.add(gGlobalTimer);
    h.add(gIsGamePaused);
    h.add(gCharacterSelections, 4);
    for (int i = 0; i < NUM_PLAYERS; i++) {
        const Player& p = gPlayers[i];
        h.add(p.type);
        h.add(p.currentRank);
        h.add(p.lapCount);
        h.add(p.pos);
        h.add(p.rotation);
        h.add(p.velocity);
        h.add(p.speed);
        h.add(p.currentItemCopy);
    }
    return h.h;
}

void EndSessionLocally(const std::string& why) {
    if (sState.sessionActive || sState.resetPending) {
        RestoreLocalState();
    }
    sState.sessionActive = false;
    sState.resetPending = false;
    sState.banner = "Session ended: " + why;
    AddLog(sState.banner);
}

void DrainClientEvents() {
    for (auto& l : sState.client.TakeLog()) {
        AddLog(l);
    }
    uint32_t df;
    if (sState.client.TakeDesync(df)) {
        sState.desynced = true;
        sState.banner = "DESYNC at frame " + std::to_string(df) +
                        ". Your games no longer match. Quit to the menu and press Start Session again.";
    }
    std::string why;
    if (sState.client.TakeSessionEnded(why)) {
        EndSessionLocally(why);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// API used by the Netplay window
// ---------------------------------------------------------------------------
std::string BuildId() {
    return NETPLAY_BUILD_ID;
}

bool Host(uint16_t port, const std::string& name, std::string& err) {
    Leave();
    sState.server = std::make_unique<Server>();
    if (!sState.server->Start(port, err)) {
        sState.server.reset();
        return false;
    }
    // The host plays too: it joins its own server like everyone else.
    if (!sState.client.Connect("127.0.0.1", port, name, BuildId(), err)) {
        sState.server.reset();
        return false;
    }
    sState.banner.clear();
    sState.desynced = false;
    return true;
}

bool Join(const std::string& host, uint16_t port, const std::string& name, std::string& err) {
    Leave();
    if (!sState.client.Connect(host, port, name, BuildId(), err)) {
        return false;
    }
    sState.banner.clear();
    sState.desynced = false;
    return true;
}

void Leave() {
    bool had = sState.client.GetState() != Client::State::Disconnected;
    sState.client.Disconnect();
    DrainClientEvents();
    if (sState.sessionActive || sState.resetPending) {
        EndSessionLocally("you left");
    }
    if (sState.server) {
        sState.server->Stop();
        sState.server.reset();
    }
    if (had) {
        AddLog("Disconnected.");
    }
}

void StartSession(int inputDelay) {
    if (!sState.client.IsLeader()) {
        return;
    }
    sState.client.RequestStart(inputDelay, BuildSnapshot());
}

Status GetStatus() {
    Status s;
    switch (sState.client.GetState()) {
        case Client::State::Disconnected:
            s.connection = "Offline";
            break;
        case Client::State::Lobby:
            s.connection = "In lobby";
            break;
        case Client::State::StartPending:
            s.connection = "Starting...";
            break;
        case Client::State::InSession:
            s.connection = "Racing online";
            break;
    }
    s.connected = sState.client.GetState() != Client::State::Disconnected;
    s.hosting = sState.server != nullptr;
    s.isLeader = sState.client.IsLeader();
    s.inSession = sState.sessionActive;
    s.lobby = sState.client.LobbyNames();
    s.myPlayer = sState.client.MyController() + 1;
    s.numPlayers = sState.client.NumPlayers();
    s.inputDelay = sState.client.InputDelay();
    s.frame = sState.client.Frame();
    s.stallMs = sState.stallMs;
    s.log = sState.log;
    s.banner = sState.banner;
    s.desynced = sState.desynced;
    return s;
}

void EndSession() {
    sState.client.RequestEnd();
}

void SendChat(const std::string& text) {
    sState.client.SendChat(text);
}

} // namespace Netplay

// ---------------------------------------------------------------------------
// Hooks called from the C game code
// ---------------------------------------------------------------------------
using namespace Netplay;

extern "C" void Netplay_OnControllersRead(OSContPad* pads) {
    Client& c = sState.client;
    if (c.GetState() == Client::State::Disconnected && !sState.sessionActive) {
        return;
    }
    c.Poll();
    DrainClientEvents();

    if (c.GetState() == Client::State::StartPending && !sState.resetPending) {
        // Session is starting. Save our own settings, take the host's, and soft-reset. The rest of the host's
        // snapshot is applied at the end of the reset (Netplay_OnReset), so everyone begins frame 0 identically.
        SaveLocalState();
        if (!ApplySnapshotSettings(c.Snapshot())) {
            AddLog("The host sent an unreadable session snapshot.");
            RestoreLocalState();
            c.Disconnect();
            return;
        }
        ForceLockstepSafeLocalSettings();
        sState.resetPending = true;
        sState.desynced = false;
        sState.banner.clear();
        CM_RequestReset();
        memset(pads, 0, sizeof(OSContPad) * 4); // this frame's result is discarded by the reset anyway
        return;
    }

    if (!sState.sessionActive) {
        return; // lobby: play normally
    }

    ForceLockstepSafeLocalSettings();

    // Desync check: hash the state produced by all previous frames.
    uint32_t frame = c.Frame();
    if (frame % kHashInterval == 0) {
        c.SendHash(frame, StateHash());
    }

    // Local input comes from this machine's first controller.
    Pad local;
    local.button = pads[0].button;
    local.stickX = pads[0].stick_x;
    local.stickY = pads[0].stick_y;
    local.rightStickX = pads[0].right_stick_x;
    local.rightStickY = pads[0].right_stick_y;

    std::vector<Pad> all;
    int waitedMs = 0;
    Client::FrameResult r;
    while ((r = c.ExchangeFrame(local, all, 100)) == Client::FrameResult::Waiting) {
        waitedMs += 100;
        if (waitedMs == 1000) {
            AddLog("Waiting for other players...");
        }
        if (waitedMs >= 15000) {
            AddLog("No input from other players for 15 seconds.");
            c.Disconnect();
            r = Client::FrameResult::Lost;
            break;
        }
    }
    sState.stallMs = c.StallMsLastSecond();
    if (r == Client::FrameResult::Lost) {
        DrainClientEvents();
        if (sState.sessionActive) {
            EndSessionLocally("lost connection");
        }
        return; // continue offline with local input
    }

    memset(pads, 0, sizeof(OSContPad) * 4);
    for (size_t i = 0; i < all.size() && i < 4; i++) {
        pads[i].button = all[i].button;
        pads[i].stick_x = all[i].stickX;
        pads[i].stick_y = all[i].stickY;
        pads[i].right_stick_x = all[i].rightStickX;
        pads[i].right_stick_y = all[i].rightStickY;
        pads[i].err_no = 0;
    }
}

extern "C" void Netplay_OnReset(void) {
    if (!sState.resetPending) {
        return;
    }
    Client& c = sState.client;
    sState.resetPending = false;
    if (c.GetState() != Client::State::StartPending || !ApplySnapshotMemory(c.Snapshot())) {
        AddLog("Could not start the session (snapshot did not match this build).");
        RestoreLocalState();
        c.Disconnect();
        return;
    }
    // ApplyPendingReset picks the next gamestate from the current one, which differs between machines.
    gGamestate = START_MENU_FROM_QUIT;
    gGamestateNext = MAIN_MENU_FROM_QUIT;
    ForceLockstepSafeLocalSettings();
    sState.sessionActive = true;
    c.BeginSession();
    AddLog("Session running. You are player " + std::to_string(c.MyController() + 1) + " of " +
           std::to_string(c.NumPlayers()) + ".");
}

extern "C" int Netplay_InSession(void) {
    return sState.sessionActive ? 1 : 0;
}

extern "C" int Netplay_BlocksSaving(void) {
    return (sState.sessionActive || sState.resetPending) ? 1 : 0;
}
