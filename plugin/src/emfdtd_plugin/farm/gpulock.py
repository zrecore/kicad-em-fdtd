"""The farm's own GPU turn manager: named locks, first come first served, held by the coordinator.

One lock per GPU (or per group of GPUs that must not run together). A holder keeps the turn until it gives it
back or its time is up; the next in the queue then gets it. Nothing here touches a process, a file or a
network: the coordinator asks for a turn before it offers a job to a device and gives it back when the
solver is known to be dead (see coordinator.py).

asyncio objects are created inside running coroutines only (the plugin must stay Python 3.9 compatible).
"""

import asyncio
import collections
import time


class Hold:
    """A turn that is held. `lost` (a future) is done if the turn ended unreturned (time ran out)."""

    def __init__(self, manager, loop, name, label, max_s, started):
        self._manager = manager
        self.name = name
        self.label = label
        self.max_s = max_s
        self.started = started
        self.loop = loop
        self.lost = loop.create_future()  # done when the turn ended without being given back
        self.released = False
        self._timer = None

    def release(self):
        """Give the turn back. Safe to call twice and after the turn expired."""
        self._manager._release(self)


class GpuLocks:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self._held = {}  # lock name -> Hold
        self._queue = collections.defaultdict(collections.deque)  # lock name -> [(label, max_s, future)]

    async def acquire(self, name, label, max_s):
        """Wait for the turn of lock `name` (first come, first served) and return the `Hold`. Cancelling the
        waiting task leaves the queue without ever holding."""
        loop = asyncio.get_running_loop()
        if name not in self._held and not self._queue[name]:
            return self._grant(loop, name, label, max_s)
        fut = loop.create_future()
        entry = (label, max_s, fut)
        self._queue[name].append(entry)
        try:
            return await fut
        except BaseException:
            if entry in self._queue[name]:
                self._queue[name].remove(entry)
            elif fut.done() and not fut.cancelled():
                fut.result().release()  # granted a moment before the cancel: pass it on
            raise

    def _grant(self, loop, name, label, max_s):
        hold = Hold(self, loop, name, label, max_s, self.clock())
        self._held[name] = hold
        hold._timer = loop.call_later(max_s, self._expire, hold)
        return hold

    def _expire(self, hold):
        if self._held.get(hold.name) is hold:
            hold.lost.set_result(True)
            self._release(hold)

    def _release(self, hold):
        if hold.released:
            return
        hold.released = True
        if hold._timer is not None:
            hold._timer.cancel()
        if self._held.get(hold.name) is hold:
            del self._held[hold.name]
            self._next(hold.name, hold.loop)

    def _next(self, name, loop):
        queue = self._queue[name]
        while queue:
            label, max_s, fut = queue.popleft()
            if fut.cancelled():
                continue
            fut.set_result(self._grant(loop, name, label, max_s))
            return

    def ahead(self, name):
        """How many turns are in front of someone who asks for `name` now (the holder and those waiting)."""
        return (name in self._held) + len(self._queue[name])

    def status(self, also=()):
        """[{"lock", "holder", "held_s", "waiting": [label...]}] for every lock in use and those in `also`."""
        now = self.clock()
        names = sorted(set(self._held) | {n for n, q in self._queue.items() if q} | set(also))
        return [
            {
                "lock": n,
                "holder": self._held[n].label if n in self._held else None,
                "held_s": round(now - self._held[n].started, 1) if n in self._held else 0.0,
                "waiting": [label for label, _, fut in self._queue.get(n, ()) if not fut.cancelled()],
            }
            for n in names
        ]
