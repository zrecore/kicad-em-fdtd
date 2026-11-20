"""KiCAD's IPC API reports lengths in integer nanometres; the solver core works in SI metres.

This module is the only place the conversion happens on the Python side (the C++ twin is
src/core/units.hpp).
"""

NM_PER_M = 1_000_000_000
MM_PER_M = 1_000


def nm_to_m(nm: int) -> float:
    return nm / NM_PER_M


def mm_to_m(mm: float) -> float:
    return mm / MM_PER_M
