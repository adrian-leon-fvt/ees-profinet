#!/bin/sh
# Run test_e2e.py (profinet-py controller) against profinet_e2e_device over a
# veth pair inside an unprivileged network namespace.
# Exits 77 (ctest SKIP) when profinet-py/pytest or user namespaces are missing.
# Usage: run_e2e.sh <path/to/profinet_e2e_device> [python] [pytest args...]
set -e
dev=$(realpath "$1")
py=${2:-python3}
[ $# -ge 2 ] && shift 2 || shift 1
here=$(dirname "$(realpath "$0")")
"$py" -c "import profinet, pytest" 2>/dev/null || {
    echo "profinet-py/pytest not importable by $py, skipping (pip install -r $here/requirements.txt)"
    exit 77
}
unshare -rn true 2>/dev/null || { echo "user netns unavailable, skipping"; exit 77; }
export PN_DEVICE="$dev"
exec unshare -rn sh -ec '
ip link set lo up
ip link add name pn_dev type veth peer name pn_ctl
ip addr add 10.0.0.2/24 dev pn_dev
ip addr add 10.0.0.1/24 dev pn_ctl
ip link set pn_dev up
ip link set pn_ctl up
py=$1; shift
exec "$py" -m pytest -p no:cacheprovider "$@"
' sh "$py" "$here/test_e2e.py" "$@"
