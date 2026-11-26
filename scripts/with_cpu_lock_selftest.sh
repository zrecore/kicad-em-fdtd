#!/bin/sh
# Self-test of scripts/with_cpu_lock.sh on a private lock path (a few seconds; run by scripts/check.sh).
cd "$(dirname "$0")/.." || exit 1
W=$PWD/scripts/with_cpu_lock.sh
mkdir -p out
T=$(mktemp -d "$PWD/out/cpulock-test.XXXXXX") || exit 1
export EMFDTD_CPU_LOCK=$T/lock
( sleep 90; echo "FAIL watchdog: the self-test hung" >&2; kill -TERM $$ ) & WD=$!
trap 'kill $WD 2>/dev/null; wait $WD 2>/dev/null; rm -rf "$T"' EXIT
fail() { echo "FAIL $*" >&2; exit 1; }
ok() { echo "ok   $*"; }

# 1. one slot: jobs never overlap and run in the order they asked
pids=
for i in 1 2 3; do
  "$W" sh -c "echo S$i >> $T/log; sleep 0.4; echo E$i >> $T/log" &
  pids="$pids $!"
  sleep 0.15
done
wait $pids
[ "$(tr '\n' ' ' < "$T/log")" = "S1 E1 S2 E2 S3 E3 " ] || fail "one slot: order was $(tr '\n' ' ' < "$T/log")"
ok "one slot: mutual exclusion and first come, first served"

# 2. two slots: two run together, the third waits for one of them
rm -f "$T/log"
pids=
for i in 1 2 3; do
  EMFDTD_CPU_SLOTS=2 "$W" sh -c "echo S$i >> $T/log; sleep 0.7; echo E$i >> $T/log" &
  pids="$pids $!"
  sleep 0.15
done
wait $pids
first_e=$(grep -n '^E' "$T/log" | head -1 | cut -d: -f1)
[ "$first_e" = 3 ] || fail "two slots: expected two starts before the first end, log $(tr '\n' ' ' < "$T/log")"
ok "two slots: two jobs overlap, the third waits"

# 3. a dead owner's slot is taken over
mkdir "$EMFDTD_CPU_LOCK.0"
dead=999990
while kill -0 $dead 2>/dev/null; do dead=$((dead - 1)); done
echo $dead > "$EMFDTD_CPU_LOCK.0/pid"; echo "no such start" > "$EMFDTD_CPU_LOCK.0/lstart"
[ "$("$W" echo taken)" = taken ] || fail "stale slot was not taken over"
ok "a dead owner's slot is taken over"

# 4. nested wrappers run directly instead of deadlocking
[ "$("$W" sh -c "$W echo inner")" = inner ] || fail "nested call"
ok "a nested call inside a held slot runs directly"

# 5. the command's exit status comes back
"$W" sh -c 'exit 7'; [ $? -eq 7 ] || fail "exit status"
ok "the exit status is returned"

# 6. a killed wrapper frees its slot and stops its command
"$W" sleep 30 & pid=$!
sleep 0.5
kill -TERM $pid; wait $pid 2>/dev/null
sleep 0.3
[ ! -d "$EMFDTD_CPU_LOCK.0" ] || fail "slot still held after the wrapper was killed"
[ "$("$W" echo after)" = after ] || fail "the next job did not run after a kill"
ok "killing the wrapper releases the slot"

# 7. status prints something sensible
"$W" --status | grep -q "slot 0: free" || fail "status"
ok "status lists the slots"
echo "with_cpu_lock: ok"
