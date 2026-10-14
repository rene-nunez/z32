#!/bin/sh
# Native checks for the tilemap/world invariants and the net wire format. No hardware
# and no Arduino needed; binaries land in $TMPDIR so the tree stays clean. The test_
# prefix is PlatformIO's suite naming, so pio test ignores this dir instead of
# trying to build it for the ESP32 (see test_ignore in platformio.ini).
set -eu
cd "$(dirname "$0")/../.."

# unquoted on purpose: word-splitting into flags
flags="-O2 -std=c++17 -Wall"

out="${TMPDIR:-/tmp}/z32_map_test"
g++ $flags -I lib/display/src -o "$out" test/test_native/map_test.cpp
"$out"

net="${TMPDIR:-/tmp}/z32_net_test"
g++ $flags -I lib/protocol/src -o "$net" test/test_native/net_test.cpp
"$net"

echo "native tests ok"
