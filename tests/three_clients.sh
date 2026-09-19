#!/bin/sh
#
# three_clients.sh -- drive required test case (5) by hand, for the screen
# capture that goes in the report.
#
# Run the server in one terminal:      ./echos 9001
# Run this in a second terminal:       ./tests/three_clients.sh 127.0.0.1 9001
#
# Three `nc` clients connect at the same time, each sends a line, and each
# prints what came back.  Screen-capture BOTH terminals: the server window
# shows three different child pids, which is the evidence that the
# connections really were simultaneous.
#
# Once your partner's `echo` client exists, run that instead -- this script
# is only a stand-in so the server can be demonstrated on its own.

HOST=${1:-127.0.0.1}
PORT=${2:-9001}
HOLD=${3:-5}          # seconds to keep each connection open

if ! command -v nc >/dev/null 2>&1; then
    echo "nc (netcat) not found; use 'make test' instead" >&2
    exit 1
fi

# Netcat flavours differ in how they close the connection after stdin EOF.
# openbsd-netcat: -N.  GNU/traditional netcat: -q 0.  Fall back to nothing
# and rely on the outer timeout.
NC_OPTS=""
NC_HELP=$(nc -h 2>&1)
if echo "$NC_HELP" | grep -q -- '-N'; then
    NC_OPTS="-N"
elif echo "$NC_HELP" | grep -q -- '-q'; then
    NC_OPTS="-q 0"
fi

# Safety net so a stubborn netcat cannot hang the demo.
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
