#!/usr/bin/env bash
# Kill the writer with SIGKILL mid-append, repeatedly, and assert that every
# acknowledged write survived. SIGKILL is the point: no destructor runs, no
# buffer is flushed on the way out, nothing gets a chance to tidy up. What
# survives is exactly what the durability path actually guaranteed.
#
#   usage: tools/crashtest.sh [rounds] [build-dir]

set -uo pipefail

ROUNDS="${1:-10}"
BUILD="${2:-build}"
WRITER="$BUILD/crashwriter"
VERIFY="$BUILD/verify"
VSIZE=200

if [[ ! -x "$WRITER" || ! -x "$VERIFY" ]]; then
  echo "build first:  cmake -S . -B $BUILD && cmake --build $BUILD -j4" >&2
  exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "crash test: $ROUNDS rounds, SIGKILL mid-append"
echo

fail=0
for ((r = 1; r <= ROUNDS; r++)); do
  LOG="$WORK/store_$r.log"
  ACK="$WORK/acked_$r.txt"

  "$WRITER" "$LOG" "$VSIZE" > "$ACK" 2>/dev/null &
  pid=$!

  # Let it get going, then kill at an unpredictable point inside an append.
  sleep 0.$((RANDOM % 5 + 5))
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null

  last="$(tail -n 1 "$ACK" 2>/dev/null || true)"
  if [[ -z "$last" ]]; then
    echo "round $r: writer produced no acknowledgements, skipping"
    continue
  fi

  if out="$("$VERIFY" "$LOG" "$last" "$VSIZE")"; then
    echo "round $r: PASS  $out"
  else
    echo "round $r: FAIL  $out"
    fail=1
  fi
done

echo
if [[ $fail -eq 0 ]]; then
  echo "all rounds passed: no acknowledged write was lost"
else
  echo "FAILURES DETECTED"
fi
exit $fail
