#!/bin/sh
# Run profinet_loopback over a veth pair inside an unprivileged network
# namespace. Exits 77 (ctest SKIP) when user namespaces are unavailable.
# Usage: loopback_netns.sh <path/to/profinet_loopback>
set -e
bin=$(realpath "$1")
unshare -rn true 2>/dev/null || { echo "user netns unavailable, skipping"; exit 77; }
exec unshare -rn sh -ec "
ip link add name pn_tx type veth peer name pn_rx
ip link set dev pn_tx up
ip link set dev pn_rx up
'$bin' pn_tx pn_rx
"
