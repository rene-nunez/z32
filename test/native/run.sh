#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."

# unquoted on purpose: word-splitting into flags
flags="-O2 -std=c++17 -Wall"

out="${TMPDIR:-/tmp}/z32_map_check"
g++ $flags -I lib/display -o "$out" test/native/map_check.cpp
"$out"

net="${TMPDIR:-/tmp}/z32_net_check"
g++ $flags -I lib/protocol -o "$net" test/native/net_check.cpp
"$net"

echo "native tests ok"
