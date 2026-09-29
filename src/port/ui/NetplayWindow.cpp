#include "NetplayWindow.h"

#include <imgui.h>
#include <libultraship/libultraship.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "enhancements/netplay/NetplayGame.h"
#include "enhancements/netplay/NetplayProtocol.h"

namespace GameUI {

static const char* PlatformName() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "Mac";
#elif defined(__linux__)
    return "Linux";
#else
    return "Other";
#endif
}

void NetplayWindow::DrawElement() {
    static char sName[32] = "";
    static char sAddress[128] = "";
    static int sPort = Netplay::kDefaultPort;
    static int sDelay = 3;
    static char sChat[128] = "";
    static std::string sError;

    if (sName[0] == '\0') {
        strncpy(sName, CVarGetString("gNetplayName", "Player"), sizeof(sName) - 1);
        strncpy(sAddress, CVarGetString("gNetplayAddress", ""), sizeof(sAddress) - 1);
        sPort = CVarGetInteger("gNetplayPort", Netplay::kDefaultPort);
        sDelay = CVarGetInteger("gNetplayInputDelay", 3);
    }

    Netplay::Status st = Netplay::GetStatus();

    ImGui::TextWrapped("Online lockstep netplay (experimental). Everyone runs the same race; up to 4 players, "
                       "shown in split screen like couch multiplayer.");
    ImGui::TextDisabled("Status: %s   |   Build: %s", st.connection.c_str(), Netplay::BuildId().c_str());
    ImGui::Separator();

    if (!st.banner.empty()) {
        ImVec4 col = st.desynced ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImVec4(1.0f, 0.85f, 0.3f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextWrapped("%s", st.banner.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    if (!st.connected && !st.inSession) {
        // ---------------- Offline: host or join ----------------
        if (ImGui::InputText("Your name", sName, sizeof(sName))) {
            CVarSetString("gNetplayName", sName);
            CVarSave();
        }
        std::string displayName = std::string(sName) + " (" + PlatformName() + ")";
        ImGui::InputInt("Port", &sPort);
        if (sPort < 1 || sPort > 65535) {
            sPort = Netplay::kDefaultPort;
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Host a game");
        ImGui::TextWrapped("Friends connect to your IP address. They need TCP port %d forwarded on your router, or "
                           "all of you on the same VPN (Tailscale, ZeroTier, Radmin VPN).",
                           sPort);
        if (ImGui::Button("Host")) {
            CVarSetInteger("gNetplayPort", sPort);
            CVarSave();
            sError.clear();
            if (!Netplay::Host((uint16_t) sPort, displayName, sError)) {
                // error shown below
            }
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Join a game");
        if (ImGui::InputText("Host address", sAddress, sizeof(sAddress))) {
            CVarSetString("gNetplayAddress", sAddress);
            CVarSave();
        }
        ImGui::BeginDisabled(sAddress[0] == '\0');
        if (ImGui::Button("Join")) {
            CVarSetInteger("gNetplayPort", sPort);
            CVarSave();
            sError.clear();
            std::string host = sAddress;
            uint16_t port = (uint16_t) sPort;
            // Accept "1.2.3.4:25564" too (but leave bare IPv6 addresses alone)
            size_t colon = host.rfind(':');
            if (colon != std::string::npos && host.find(':') == colon) {
                port = (uint16_t) atoi(host.c_str() + colon + 1);
                host = host.substr(0, colon);
            }
            Netplay::Join(host, port, displayName, sError);
        }
        ImGui::EndDisabled();

        if (!sError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", sError.c_str());
            ImGui::PopStyleColor();
        }
    } else {
        // ---------------- Lobby / session ----------------
        ImGui::SeparatorText(st.inSession ? "Racing" : "Lobby");
        for (size_t i = 0; i < st.lobby.size(); i++) {
            ImGui::BulletText("Player %d: %s%s", (int) i + 1, st.lobby[i].c_str(), i == 0 ? "  [host]" : "");
        }

        if (st.inSession) {
            ImGui::Text("You are player %d of %d. Input delay: %d frames. Frame %u.", st.myPlayer, st.numPlayers,
                        st.inputDelay, st.frame);
            // Lag indicator: time per second spent waiting on the network
            if (st.stallMs < 50) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Connection: smooth");
            } else if (st.stallMs < 250) {
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                                   "Connection: some stutter (%d ms/s waiting). Try a higher input delay.",
                                   st.stallMs);
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                   "Connection: laggy (%d ms/s waiting). Raise input delay next session.",
                                   st.stallMs);
            }
            ImGui::TextWrapped("In the game's menu, pick %d players so every kart has a driver. Player 1's controller "
                               "is the host's, player 2's is the next person to join, and so on.",
                               st.numPlayers);
        } else if (st.isLeader) {
            ImGui::SliderInt("Input delay (frames)", &sDelay, Netplay::kMinInputDelay, Netplay::kMaxInputDelay);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("How far ahead inputs are sent. Each frame is 33 ms.\n"
                                  "2-3: same city / good connection. 4-6: across the country. 7+: overseas.\n"
                                  "Too low = stuttering, too high = controls feel sluggish.");
            }
            if (ImGui::Button("Start Session")) {
                CVarSetInteger("gNetplayInputDelay", sDelay);
                CVarSave();
                Netplay::StartSession(sDelay);
            }
            ImGui::TextDisabled("Starting resets everyone to the title screen with your settings and save data.");
        } else {
            ImGui::Text("Waiting for the host to start the session...");
        }

        ImGui::Spacing();
        if (st.inSession && st.isLeader) {
            if (ImGui::Button("End Session")) {
                Netplay::EndSession();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Everyone goes back to the lobby (and gets their own settings back).\n"
                                  "Use this after a desync, or to change the input delay.");
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(st.hosting ? "Stop hosting" : "Leave")) {
            Netplay::Leave();
        }
    }

    // ---------------- Log / chat ----------------
    ImGui::Spacing();
    ImGui::SeparatorText("Messages");
    ImGui::BeginChild("netplay_log", ImVec2(0, 140), true);
    for (const std::string& l : st.log) {
        ImGui::TextWrapped("%s", l.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    if (st.connected) {
        ImGui::SetNextItemWidth(-60);
        bool send = ImGui::InputText("##chat", sChat, sizeof(sChat), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        send |= ImGui::Button("Send");
        if (send && sChat[0] != '\0') {
            Netplay::SendChat(sChat);
            sChat[0] = '\0';
        }
    }
}

} // namespace GameUI
