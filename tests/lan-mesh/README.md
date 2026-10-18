# LAN mesh tests

Plays simulated 3-player LAN games through the game's real
`GeneralsMD/Code/GameEngine/Source/GameNetwork/GeneralsOnline/LanMesh.cpp` and real GameNetworkingSockets,
on Linux, without the rest of the game.

- `stubs/` holds small stand-ins for the engine types `LanMesh.cpp` uses (LAN room, global data, transport base).
- `test_player.cpp` is one player. It runs the game room phase (the host's start check) and then a match,
  sending numbered game packets of varying size to every other player and checking they all arrive, intact and in order.
- `run.sh` puts each player in its own network namespace on a virtual switch, like three PCs on a LAN.

Scenarios: clean network, stress (far more traffic than a real match), 10% packet loss with 40 ms lag,
a player joining late, a player with different game files, a player in the room who never runs this build,
a player who crashes and rejoins, and a player whose UDP port 8088 is taken.

## Running

```bash
sudo apt-get install g++ cmake ninja-build libssl-dev libprotobuf-dev protobuf-compiler iproute2 python3
tests/lan-mesh/build.sh
sudo tests/lan-mesh/run.sh
```

Logs are in `tests/lan-mesh/.build/out`. The LAN Build workflow runs these tests on every push.
