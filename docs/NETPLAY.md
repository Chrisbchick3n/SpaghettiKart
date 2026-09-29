# Online Netplay (experimental)

Race up to 4 friends online in real time. This is **lockstep netplay**, the same approach emulator netplay uses:
every player's game runs the exact same race, and the only thing sent over the internet is each player's
controller input. Items, bumps, shells and CPU karts all behave identically for everyone.

You'll see the race in split screen, just like 2-4 player couch multiplayer on the N64.

## What everyone needs

- A SpaghettiKart netplay build made from the **same source commit** as everyone else's (the Windows, Mac,
  Linux and Android downloads from one build run all match). The game refuses to connect mismatched builds.
- Their own **US Mario Kart 64 ROM**, set up the normal SpaghettiKart way.
- Mixing systems (Windows, Mac, Linux, Android) is supported but still experimental; see "Phones and PCs
  together" below.
- The **same mods / custom tracks** installed (or none).

## Playing

1. Press **Esc** to open the settings menu → **Online** → **Open Netplay Window**.
2. Type your name.
3. **Host:** press **Host**. Give your friends your IP address.
   **Friends:** type the host's IP (e.g. `203.0.113.7` or `203.0.113.7:25564`) and press **Join**.
4. When everyone is listed in the lobby, the host picks an **input delay** and presses **Start Session**.
   Everyone's game resets to the title screen using the host's settings and save data.
5. In the game's own menus, choose **2P / 3P / 4P** to match the number of people. Player 1 is the host,
   player 2 is the first friend who joined, and so on. Anyone can navigate the menus.
6. Race! The host can press **End Session** to return everyone to the lobby (e.g. to change the input delay).
   To quit, press **Leave** / **Stop hosting**.

### Input delay

Inputs are sent a few frames ahead so they arrive in time. One frame is 33 ms.

| Connection | Suggested delay |
| --- | --- |
| Same house / same city | 2-3 |
| Same country | 4-6 |
| Different continents | 7-10 |

Too low → the game stutters while it waits for other players. Too high → controls feel sluggish.
The Netplay window shows a connection indicator; if it says "stutter" or "laggy", start a new session with a
higher delay (host: **End Session**, change the slider, **Start Session**).

## Android (phones and tablets)

The `netplay-android` branch builds an Android app with netplay (**SpaghettiKart Netplay**, package
`com.izzy.kart.netplay`, based on izzy2lost's Android port). It installs next to the normal SpaghettiKart app.

- Install the `spaghettikart-netplay.apk` from the **Android Netplay Build** run (allow "install unknown apps").
- On first launch, pick your US Mario Kart 64 ROM, the same as the normal Android app.
- Tap the on-screen **Menu** button → **Online** → **Open Netplay Window**. Tapping a text box brings up the
  keyboard.
- Joining is the easy part on a phone. Hosting from mobile data usually won't work (carriers block incoming
  connections); host from a PC, use the relay, or have everyone use Tailscale (it has an Android app).
- Keep the app in the foreground. If Android pauses it (switching apps, screen off), everyone else's race waits
  for you, and after 15 seconds you're dropped.

### Phones and PCs together (cross-play)

Builds of the **same commit** can play together on any system. Use the Windows/Mac/Linux downloads from the
`netplay-android` branch's **GenerateBuilds** run (not the `netplay` branch) together with its APK. The Netplay
window shows the build id; it must match for everyone.

The physics is designed to give identical results on phone (ARM) and PC (x86) chips, but this hasn't been
proven in a real race yet. If you get a **DESYNC** warning only when mixing phones and PCs, please report it.

## Hosting: letting friends reach you

The host listens on **TCP port 25564** (changeable in the window). Friends outside your home network can only
connect if one of these is true:

- **Easiest: a VPN app** like [Tailscale](https://tailscale.com), ZeroTier or Radmin VPN. Everyone installs it and
  joins the same network, then friends use the host's VPN IP. No router changes needed.
- **Port forwarding:** forward TCP 25564 on the host's router to the host's PC, and allow SpaghettiKart through
  the firewall. Friends use the host's public IP.
- **Dedicated relay:** run `spaghetti-netplay-relay` on any always-on machine or cheap VPS with the port open.
  Everyone (including the "host") uses **Join** to connect to it. The first person to join picks the settings and
  starts the session.

## Desyncs

Every 2 seconds the games compare a fingerprint of the race (kart positions, speeds, the random-number state...).
If they don't match, everyone sees a red **DESYNC** message: the races are no longer identical, so what you see
isn't what your friends see. The host presses **End Session**, then **Start Session** again.

Common causes:

- Different builds, mods or custom tracks
- Different operating systems (floating-point math can differ slightly)
- A bug. Please report it with the frame number shown.

## What's synced and what isn't

At session start the host's gameplay settings (Enhancements, Cheats, Rulesets such as CPU difficulty, custom CC,
item boxes, traffic counts), save data (unlocks), win counters and menu state are copied to everyone. Your own
settings and save data come back when the session ends. **Nothing is saved to your save file during an online
session.**

Graphics, audio and control settings stay your own.

While a session is running, freecam, the debug game-speed tool and the reset button are disabled, because they
would make your game different from everyone else's.

## Known limitations

- Split screen only (everyone sees all players' views).
- If a player's connection stalls, everyone's game pauses until their input arrives. After 15 seconds with no
  input, the session ends for the waiting players.
- If a player leaves mid-race, their kart stops (neutral input) and the race continues for everyone else.
- Max 4 human players (the N64 game's limit).

## For developers

- `src/enhancements/netplay/` — protocol, sockets, host server, client lockstep engine, game glue.
- `src/port/ui/NetplayWindow.cpp` — the ImGui window.
- Hooks: `read_controllers()` in `src/main.c`, the end of `ApplyPendingReset()` in `src/port/Game.cpp`,
  EEPROM writes in `src/save.c`.
- `tools/netplay/` — standalone relay server and a headless loopback test (3-4 simulated players; checks that
  games stay identical, that desyncs are detected, and that dropped players don't stop the race):

  ```sh
  cmake -S tools/netplay -B build-netplay && cmake --build build-netplay
  ./build-netplay/netplay-loopback-test
  ```
