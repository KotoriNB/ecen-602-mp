#!/bin/sh
#
# three_clients.sh -- required test case 5, by hand, for the screen capture.
#
#   terminal 1:  ./echos 9001
#   terminal 2:  ./tests/three_clients.sh 127.0.0.1 9001
#
# Capture both windows: the server showing three different child pids is the
# evidence that the connections really were simultaneous.

HOST=${1:-127.0.0.1}
PORT=${2:-9001}
HOLD=${3:-5}          # seconds to keep each connection open

if ! command -v nc >/dev/null 2>&1; then
    echo "nc (netcat) not found; use 'make test' instead" >&2
    exit 1
fi

# Netcat flavours differ in how they close after stdin EOF: openbsd uses -N,
# GNU uses -q.
NC_OPTS=""
NC_HELP=$(nc -h 2>&1)
if echo "$NC_HELP" | grep -q -- '-N'; then
    NC_OPTS="-N"
elif echo "$NC_HELP" | grep -q -- '-q'; then
    NC_OPTS="-q 0"
fi

if command -v timeout >/dev/null 2>&1; then
    GUARD="timeout $((HOLD + 5))"
else
    GUARD=""
fi

echo "connecting three clients to $HOST:$PORT (holding ${HOLD}s each) ..."

i=1
while [ "$i" -le 3 ]; do
    (
        {
            printf 'client %s: hello\n' "$i"
            sleep "$HOLD"
        } | $GUARD nc $NC_OPTS "$HOST" "$PORT" | sed "s/^/[client $i] /"
    ) &
    sleep 0.3
    i=$((i + 1))
done

wait
echo "all three clients finished (each sent EOF, server children exited)"
