#!/bin/sh
# Run a command while holding a slot of the machine's CPU queue, so heavy CPU jobs (sanitizer builds, the slow
# test suite, long CPU solves, anything whose timing matters) take turns instead of piling onto the same cores.
# The GPU has its own lock (with_gpu_lock.sh); this one is for the CPU and is independent of it.
#
#   scripts/with_cpu_lock.sh [-n SLOTS] [-p NICE] [-l LABEL] COMMAND [ARGS...]
#   scripts/with_cpu_lock.sh --status
#
# Protocol (the same shape as the GPU lock): a fair ticket queue in $LOCK.queue, one directory per slot
# ($LOCK.0 .. $LOCK.N-1) holding the owner's pid and start time; a dead owner (pid gone or reused) is taken
# over; the command runs at NICE (default 10) and its exit status is returned. A command started from inside
# a held slot (EMFDTD_CPU_HELD is set) runs directly, so nested wrappers cannot deadlock.
# Environment: EMFDTD_CPU_LOCK (path prefix, default /tmp/emfdtd_cpu.lock: a lock must live where every
# process of the machine sees it), EMFDTD_CPU_SLOTS (default 1), EMFDTD_CPU_NICE (default 10).
LOCK=${EMFDTD_CPU_LOCK:-/tmp/emfdtd_cpu.lock}
QUEUE=$LOCK.queue
SLOTS=${EMFDTD_CPU_SLOTS:-1}
NICE=${EMFDTD_CPU_NICE:-10}
LABEL=

lstart_of() { ps -o lstart= -p "$1" 2>/dev/null | sed 's/^ *//'; }
alive() { [ -n "$1" ] || return 1; kill -0 "$1" 2>/dev/null || return 1; [ -z "$2" ] || [ "$(lstart_of "$1")" = "$2" ]; }

status() {
  held=0
  i=0
  while [ "$i" -lt "$SLOTS" ]; do
    if [ -d "$LOCK.$i" ]; then
      pid=$(cat "$LOCK.$i/pid" 2>/dev/null)
      if alive "$pid" "$(cat "$LOCK.$i/lstart" 2>/dev/null)"; then
        echo "slot $i: held by pid $pid: $(cat "$LOCK.$i/label" 2>/dev/null)"
        held=$((held + 1))
      else
        echo "slot $i: stale (pid ${pid:-?} gone)"
      fi
    else
      echo "slot $i: free"
    fi
    i=$((i + 1))
  done
  n=0
  for t in $(ls "$QUEUE" 2>/dev/null); do
    pid=$(cat "$QUEUE/$t/pid" 2>/dev/null)
    if alive "$pid" "$(cat "$QUEUE/$t/lstart" 2>/dev/null)"; then
      n=$((n + 1))
      echo "queued $n: pid $pid: $(cat "$QUEUE/$t/label" 2>/dev/null)"
    fi
  done
  [ "$n" -gt 0 ] || echo "queue empty"
}

while [ $# -gt 0 ]; do
  case "$1" in
    -n) SLOTS=$2; shift 2 ;;
    -p) NICE=$2; shift 2 ;;
    -l) LABEL=$2; shift 2 ;;
    --status) status; exit 0 ;;
    --) shift; break ;;
    -*) echo "with_cpu_lock: unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ $# -gt 0 ] || { echo "usage: with_cpu_lock.sh [-n SLOTS] [-p NICE] [-l LABEL] COMMAND [ARGS...] | --status" >&2; exit 2; }
if [ -n "${EMFDTD_CPU_HELD:-}" ]; then exec "$@"; fi
[ -n "$LABEL" ] || LABEL="$*"

mkdir -p "$QUEUE" 2>/dev/null
seq=$(ls "$QUEUE" 2>/dev/null | tail -1 | sed 's/^0*//')
seq=$(( ${seq:-0} + 1 ))
while :; do
  TICKET=$QUEUE/$(printf %010d "$seq")
  mkdir "$TICKET" 2>/dev/null && break
  seq=$((seq + 1))
done
echo $$ > "$TICKET/pid"; lstart_of $$ > "$TICKET/lstart"; echo "$LABEL" > "$TICKET/label"
SLOT=
child=
cleanup() { [ -z "$SLOT" ] || rm -rf "$SLOT"; rm -rf "$TICKET"; }
trap 'cleanup' EXIT
trap '[ -z "$child" ] || kill -TERM "$child" 2>/dev/null; cleanup; exit 130' INT TERM

told=0
while :; do # wait until every live ticket ahead of ours has got its slot
  ahead=0
  for t in $(ls "$QUEUE" 2>/dev/null); do
    [ "$QUEUE/$t" = "$TICKET" ] && break
    tp=$(cat "$QUEUE/$t/pid" 2>/dev/null)
    if [ -n "$tp" ] && ! alive "$tp" "$(cat "$QUEUE/$t/lstart" 2>/dev/null)"; then rm -rf "${QUEUE:?}/$t"; continue; fi
    ahead=$((ahead + 1))
  done
  [ "$ahead" -eq 0 ] && break
  [ "$told" -eq 0 ] && echo "with_cpu_lock: $ahead job(s) ahead in the queue" >&2
  told=1; sleep 1
done
while [ -z "$SLOT" ]; do # take the first free slot, clearing a dead owner's
  i=0
  while [ "$i" -lt "$SLOTS" ]; do
    if mkdir "$LOCK.$i" 2>/dev/null; then SLOT=$LOCK.$i; break; fi
    owner=$(cat "$LOCK.$i/pid" 2>/dev/null)
    if [ -n "$owner" ] && ! alive "$owner" "$(cat "$LOCK.$i/lstart" 2>/dev/null)"; then rm -rf "$LOCK.$i"; continue; fi
    i=$((i + 1))
  done
  [ -n "$SLOT" ] && break
  [ "$told" -eq 0 ] && echo "with_cpu_lock: all $SLOTS slot(s) busy; waiting" >&2
  told=1; sleep 1
done
echo $$ > "$SLOT/pid"; lstart_of $$ > "$SLOT/lstart"; echo "$LABEL" > "$SLOT/label"
rm -rf "$TICKET" # the next in line may now look for a slot

EMFDTD_CPU_HELD=1 nice -n "$NICE" "$@" &
child=$!
wait "$child"
rc=$?
child=
exit "$rc"
