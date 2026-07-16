"""DCX-004 uses one MT44 frame; no cross-frame reassembly is specified."""


def requires_reassembly() -> bool:
    return False

