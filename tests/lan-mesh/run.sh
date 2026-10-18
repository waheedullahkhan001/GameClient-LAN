#!/usr/bin/env bash
# Plays simulated 3-player LAN games through the real LanMesh.cpp. Each player runs in its own network
# namespace on a virtual switch, like three PCs on a LAN. Needs root (network namespaces) and iproute2.
# Exit code is 0 only if every scenario passes. Logs go to $WORK/out.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-$HERE/.build}"
BIN="$WORK/test_player"
OUT="$WORK/out"
PLAYERS=10.9.0.1,10.9.0.2,10.9.0.3
FAILED=0

rm -rf "$OUT"
mkdir -p "$OUT"

teardown() {
	for ns in lm_br lm_p1 lm_p2 lm_p3; do
		ip netns del "$ns" 2>/dev/null || true
	done
}

setup() {
	teardown
	ip netns add lm_br
	ip -n lm_br link add br0 type bridge
	ip -n lm_br link set br0 up
	for i in 1 2 3; do
		ip netns add "lm_p$i"
		ip link add "lm_v$i" type veth peer name "lm_b$i"
		ip link set "lm_v$i" netns "lm_p$i"
		ip link set "lm_b$i" netns lm_br
		ip -n lm_br link set "lm_b$i" master br0
		ip -n lm_br link set "lm_b$i" up
		ip -n "lm_p$i" addr add "10.9.0.$i/24" dev "lm_v$i"
		ip -n "lm_p$i" link set "lm_v$i" up
		ip -n "lm_p$i" link set lo up
	done
}

run() { # log name, player number, extra args...
	local name=$1 n=$2
	shift 2
	ip netns exec "lm_p$n" "$BIN" --me "10.9.0.$n" --players "$PLAYERS" --signal "$SIG" "$@" > "$OUT/$SCEN-$name.log" 2>&1
}

report() {
	local codes="$*"
	if [ "$codes" = "0 0 0" ]; then
		echo "PASS  $SCEN"
	else
		echo "FAIL  $SCEN (exit codes $codes)"
		grep -h "TEST" "$OUT/$SCEN"-*.log | grep -v "bad packet" | sed 's/^/      /'
		FAILED=1
	fi
	rm -rf "$SIG"
}

scenario() { # name, then the extra args of host, player 2 and player 3 separated by ';'
	SCEN=$1
	shift
	SIG=$(mktemp -d)
	IFS=';' read -r A1 A2 A3 <<< "$*"
	run p1 1 --host $A1 & P1=$!
	run p2 2 $A2 & P2=$!
	run p3 3 $A3 & P3=$!
	wait $P1; R1=$?; wait $P2; R2=$?; wait $P3; R3=$?
	report $R1 $R2 $R3
}

trap teardown EXIT
setup

scenario clean     "--burst 1;--burst 1;--burst 1"
scenario stress    "--burst 8;--burst 8;--burst 8"
scenario lossy     "--burst 1 --loss 10 --lag 40;--burst 1 --loss 10 --lag 40;--burst 1 --loss 10 --lag 40"
scenario late-join "--burst 1;--burst 1;--burst 1 --late 5000"
# Player 3 runs different game files, so the host must refuse to start.
SCEN=mismatch
SIG=$(mktemp -d)
run p1 1 --host --expect-not-ready "different game version" --timeout 15000 & P1=$!
run p2 2 --expect-no-start --timeout 15000 & P2=$!
run p3 3 --expect-no-start --exe 99999999 --timeout 15000 & P3=$!
wait $P1; R1=$?; wait $P2; R2=$?; wait $P3; R3=$?
report $R1 $R2 $R3

# Player 3 is in the room but never runs the LAN build, so nobody can reach them.
SCEN=missing-player
SIG=$(mktemp -d)
run p1 1 --host --expect-not-ready "cannot reach P3" --timeout 15000 & P1=$!
run p2 2 --expect-no-start --timeout 15000 & P2=$!
wait $P1; R1=$?; wait $P2; R2=$?
report $R1 $R2 0

# Player 3 connects, quits without saying goodbye, then starts again.
SCEN=rejoin
SIG=$(mktemp -d)
run p1 1 --host --burst 1 --start-after 8000 & P1=$!
run p2 2 --burst 1 & P2=$!
run p3-first 3 --exit-when-connected
run p3 3 --burst 1 & P3=$!
wait $P1; R1=$?; wait $P2; R2=$?; wait $P3; R3=$?
report $R1 $R2 $R3

# Another program holds UDP 8088 on player 3, so nobody can dial player 3. Player 3 dialling out must be enough.
SCEN=port-busy
SIG=$(mktemp -d)
ip netns exec lm_p3 python3 -c "
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('0.0.0.0', 8088))
time.sleep(60)" & HOLDER=$!
sleep 1
run p1 1 --host --burst 1 & P1=$!
run p2 2 --burst 1 & P2=$!
run p3 3 --burst 1 & P3=$!
wait $P1; R1=$?; wait $P2; R2=$?; wait $P3; R3=$?
kill $HOLDER 2>/dev/null
if ! grep -q "Could not listen" "$OUT/port-busy-p3.log"; then
	echo "FAIL  port-busy (the port was not actually blocked)"
	FAILED=1
fi
report $R1 $R2 $R3

exit $FAILED
