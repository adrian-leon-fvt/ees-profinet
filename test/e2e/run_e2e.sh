#!/bin/sh
# Run test_e2e.py (profinet-py controller) against profinet_e2e_device inside
# an unprivileged network namespace; the fixture adds a veth pair to a child
# namespace per test.
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
py=$1; shift
exec "$py" -m pytest -p no:cacheprovider "$@"
' sh "$py" "$here/test_e2e.py" "$@"
