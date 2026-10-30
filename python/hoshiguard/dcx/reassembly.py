"""DCX-004 は単一 MT44 を使い、フレームをまたぐ再構成を規定しない。"""


def requires_reassembly() -> bool:
    return False

