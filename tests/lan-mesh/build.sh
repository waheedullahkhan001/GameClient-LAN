#!/usr/bin/env bash
# Builds the LAN mesh test player on Linux: the game's real LanMesh.cpp, the engine stand-ins in stubs/,
# and GameNetworkingSockets at the exact commit the game's vcpkg port uses.
# Needs: g++, cmake, ninja, libssl-dev, libprotobuf-dev, protobuf-compiler.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WORK="${WORK:-$HERE/.build}"

# Keep in sync with the gamenetworkingsockets port in the registry named in vcpkg.json.
GNS_URL="https://github.com/tintinhamans/GNS"
GNS_COMMIT="fa5fd2dcb396510d3d3f56fac34b0f6410b57a8b"

mkdir -p "$WORK"

if [ ! -f "$WORK/gns/build/src/libGameNetworkingSockets_s.a" ]; then
	rm -rf "$WORK/gns"
	git init -q "$WORK/gns"
	git -C "$WORK/gns" fetch -q --depth 1 "$GNS_URL" "$GNS_COMMIT"
	git -C "$WORK/gns" checkout -q FETCH_HEAD
	cmake -S "$WORK/gns" -B "$WORK/gns/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
		-DBUILD_STATIC_LIB=ON -DBUILD_SHARED_LIB=OFF -DENABLE_ICE=OFF -DUSE_CRYPTO=OpenSSL > "$WORK/gns-configure.log"
	ninja -C "$WORK/gns/build" GameNetworkingSockets_s > "$WORK/gns-build.log"
fi

g++ -std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -DSTEAMNETWORKINGSOCKETS_STATIC_LINK \
	-I "$HERE/stubs" \
	-I "$REPO/GeneralsMD/Code/GameEngine/Include" \
	-I "$WORK/gns/include" \
	"$REPO/GeneralsMD/Code/GameEngine/Source/GameNetwork/GeneralsOnline/LanMesh.cpp" \
	"$HERE/test_player.cpp" \
	-o "$WORK/test_player" \
	-L "$WORK/gns/build/src" -lGameNetworkingSockets_s -lprotobuf -lssl -lcrypto -lpthread

echo "Built $WORK/test_player"
