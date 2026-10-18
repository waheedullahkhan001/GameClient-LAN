# Generals Online networking for LAN games

**Status: experimental.** Please report what works and what does not.

## Why

Generals Online has two multiplayer paths:

| | **Online** menu | **Network** (LAN) menu |
|---|---|---|
| Game traffic | Valve GameNetworkingSockets: reliable, fast resend of lost packets | Legacy 2003 UDP code: a lost packet waits about 2 seconds before it is resent |
| When players connect | In the lobby, before the match. Start is blocked until everyone is connected | Only when the match starts |
| Network tuning (run-ahead slack, latency model) | Sent by the Generals Online server | Old defaults |
| Latency at match start | Measured | Guessed (200 ms) |
| Version check | Server makes sure everyone runs the same build | None, so mixed builds join and then fail at load |
| In-game chat | Yes | Disabled |

So LAN games miss all the online reliability work. This fork brings it to the **Network** menu, with no server needed.

## What this fork changes

All changes are marked with `GO_LAN` in the code.

- **Same transport as online.** LAN game traffic goes through GameNetworkingSockets, the library the Online menu uses,
  but peers connect straight to each other by IP address (UDP port 8088, the same port LAN games always used).
  No signalling server, STUN or TURN is involved.
  See `GeneralsMD/Code/GameEngine/Source/GameNetwork/GeneralsOnline/LanMesh.cpp`.
- **Connect in the game room.** Players connect to each other as soon as they join a LAN game room. The lobby chat
  shows who is connected. The host can only start once every player is connected to every other player,
  like the online "full mesh" check.
- **Version check.** Players exchange their exe and INI checksums. A player with different game files is reported
  in the lobby chat and the game cannot start.
- **Same tuning as online.** Run-ahead slack 30%, latency model 2 (highest of the game's and the library's latency),
  and the match starts from the latency measured in the lobby. These are the values the Generals Online service
  sends to online games today.
- **In-game chat works in LAN games**, LAN games use your FPS limit setting like online games, and the **F10**
  network stats overlay (ping and connection quality per player) shows LAN connections too.

**Gameplay change:** cargo planes sent by a USA Supply Drop Zone (all USA generals) cannot be targeted,
so the supplies always arrive. This uses the game's own "masked" status, the one mines use.
It applies to everything played with this exe, so LAN replays stay in sync.

Things that already applied to LAN games in Generals Online, and still do: 60 Hz game logic, widescreen and camera
fixes, larger network packets and the other client fixes.

Things that stay online only: login, ladders, stats, matchmaking, anti-cheat, replay upload, and the host's
`/maxcameraheight` limit (online it is shared through the lobby server).

## Testing

`tests/lan-mesh` plays simulated 3-player LAN games through the real `LanMesh.cpp` and GameNetworkingSockets on Linux:
clean network, heavy traffic, 10% packet loss with 40 ms lag, a late joiner, mismatched game files, a player who never
runs this build, a crash and rejoin, and a blocked port. The LAN Build workflow runs them on every push.
Real matches on real PCs are still untested.

## Installing

Every player, the host included, must use **exactly the same files**. Mixing with the normal Generals Online exe,
the Super Patch or 1.04 does not work, and the game room will say so.

1. Install Generals Online normally with its launcher once, so the game folder has its data files.
2. Download the zip from the [latest release](https://github.com/waheedullahkhan001/GameClient-LAN/releases/latest).
3. Extract it into your Generals Online game folder (the one with `GeneralsOnlineZH.exe`).
   It adds `GeneralsOnlineZH_LAN.exe` and leaves the official exe untouched for online play.
4. Run `Allow-Through-Firewall.bat` once. Virtual LAN adapters are often marked "public" and get blocked otherwise.
5. Start `GeneralsOnlineZH_LAN.exe` and use the **Network** menu.

Development builds from the `lan` branch are on the
[LAN Build workflow](https://github.com/waheedullahkhan001/GameClient-LAN/actions/workflows/lan-build.yml) page
(the `GeneralsMD-win32-vcpkg` artifact, rename `GeneralsOnlineZH.exe` to `GeneralsOnlineZH_LAN.exe`).

## Updating from Generals Online

`main` is based on Generals Online at commit `6ff4044` (October 2026). Newer Generals Online changes need a merge
into this fork and a new build. Every player then has to update to the new release together.

## In the game room

The lobby chat tells you what the connections are doing:

| Message | Meaning |
|---|---|
| Connected to *name*. | Ready to play with that player. |
| Cannot reach *name*. | After 10 seconds still no connection. That player probably runs another exe, or Windows Firewall blocks it (run `Allow-Through-Firewall.bat`). |
| *name* is running a different game version. | Their exe or game data differs. Everyone needs the same release. |
| Lost connection to *name*, reconnecting... | The connection dropped; it retries by itself. |
| Network warning: UDP port 8088 is busy on this PC. | Another program (often a second copy of the game) uses the port. Others can still reach you through your own outgoing connection. |

The host's **Start** only works once everyone is connected to everyone, and it is checked again when the countdown ends.

## Reporting problems

The log is usually at `Documents\Command and Conquer Generals Zero Hour Data\GeneralsOnlineData\GeneralsOnline.log`.
Lines from this feature start with `[LAN MESH]`. Please attach the log from every player in the match.
