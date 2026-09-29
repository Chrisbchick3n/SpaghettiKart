# Online Netplay (experimental)

Race up to 4 friends online in real time. This is **lockstep netplay**, the same approach emulator netplay uses:
every player's game runs the exact same race, and the only thing sent over the internet is each player's
controller input. Items, bumps, shells and CPU karts all behave identically for everyone.

You'll see the race in split screen, just like 2-4 player couch multiplayer on the N64.

## What everyone needs

- The **exact same SpaghettiKart netplay build** (download the same file, don't mix builds). The game refuses
  to connect mismatched builds.
- Their own **US Mario Kart 64 ROM**, set up the normal SpaghettiKart way.
- Ideally the **same operating system** as the host. Mixing Windows/Mac/Linux may work, but tiny math differences
  between systems can cause a desync (see below).
- The **same mods / custom tracks** installed (or none).

## Playing

1. Press **Esc** to open the settings menu → **Online** → **Open Netplay Window**.
2. Type your name.
3. **Host:** press **Host a game**. You get a short **game code** (like `K7QXA`). Press **Copy** and send it to
   your friends.
   **Friends:** type the code in the box and press **Join a game**.
   Nobody needs port forwarding, a VPN, or an IP address: everyone connects to the online server, which passes
   the inputs along.
4. When everyone is listed in the lobby, the host picks an **input delay** and presses **Start Session**.
   Everyone's game resets to the title screen using the host's settings and save data.
5. In the game's own menus, choose **2P / 3P / 4P** to match the number of people. Player 1 is the host,
   player 2 is the first friend who joined, and so on. Anyone can navigate the menus.
6. Race! The host can press **End Session** to return everyone to the lobby (e.g. to change the input delay).
   To quit, press **Leave**. The code stops working once everyone has left.

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

## The online server (for whoever publishes the builds)

**Host a game / Join a game** go through a small relay server, `spaghetti-netplay-relay`. Players only ever
connect *out* to it, which works on every home network, including mobile, satellite and carrier-grade NAT, so
players never touch their routers. One relay serves many groups at once, each with its own code.

The relay has to run somewhere with a public address. Set it up **once**:

- **Fly.io (easiest, about $4/month):** install `flyctl`, then from `tools/netplay/`:
  `fly launch --no-deploy --copy-config --name my-spaghetti-relay`, `fly ips allocate-v4`, `fly deploy`.
  The address is `my-spaghetti-relay.fly.dev`.
- **Any Linux VPS, or Oracle Cloud's Always Free VM (free):** run
  `curl -fsSL https://raw.githubusercontent.com/Chrisbchick3n/SpaghettiKart/netplay/tools/netplay/install-relay.sh | sudo sh`.
  It builds the relay, runs a self-test, starts it on boot, and prints the address. Also open **TCP 25564** in
  the provider's firewall (Oracle: VCN → Security List → Ingress rule).
- **Docker anywhere:** `docker build -t spaghetti-relay tools/netplay` then
  `docker run -d --restart unless-stopped -p 25564:25564 spaghetti-relay`.
- **Your own always-on PC:** run `spaghetti-netplay-relay` and forward TCP 25564 on *your* router. Only the
  person running the relay does this, once. It only works while that PC is on.

Then build the address into the game so players don't have to type anything: in GitHub, open
**Settings → Secrets and variables → Actions → Variables** and add `NETPLAY_RELAY` = the address (`host` or
`host:port`). Every build after that has it. (Local builds: `-DNETPLAY_DEFAULT_RELAY=host`.) Players can
override it in the Netplay window under **Advanced → Online server**.

Each CI run also uploads a ready-made Linux relay binary (`spaghetti-netplay-relay-linux-x64`).

Relay bandwidth is tiny (a few KB/s per player), so the smallest server is plenty. Put it near your players:
every input makes a round trip through it, so a server far away means you need a higher input delay.

### Direct connection (no server)

Under **Advanced** you can still host straight from your PC with **Host on this PC** (port **25564**, changeable).
Friends use **Join by address** with your IP. This needs TCP 25564 forwarded on the host's router, or everyone on
the same VPN ([Tailscale](https://tailscale.com), ZeroTier, Radmin VPN). Plain **Join by address** pointed at a
relay also works; everyone who does that shares one lobby.

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
- `tools/netplay/` — the online relay (`RoomRelay`: room codes, one lockstep `Server` per room), its deployment
  files (Dockerfile, fly.toml, install-relay.sh), and a headless loopback test (3-4 simulated players, direct and
  through the relay; checks that games stay identical, desyncs are detected, dropped players don't stop the race,
  rooms stay separate and bad codes are refused):

  ```sh
  cmake -S tools/netplay -B build-netplay && cmake --build build-netplay
  ./build-netplay/netplay-loopback-test
  ```
