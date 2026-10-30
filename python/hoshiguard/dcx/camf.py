"""CAMF Issue 1.2 のフィールド抽出。bit 0 は L1S の MSB。"""

from __future__ import annotations

from ..l1s import read_bits
from .models import CamfFields

CAMF_TRACE = {
    "A1": (24, 2), "A2": (26, 9), "A3": (35, 5), "A4": (40, 7),
    "A5": (47, 2), "A6": (49, 1), "A7": (50, 14), "A8": (64, 2),
    "A9": (66, 1), "A10": (67, 3), "A11": (70, 10),
    "A12": (80, 16), "A13": (96, 17), "A14": (113, 5),
    "A15": (118, 5), "A16": (123, 6), "A17": (129, 2),
    "A18": (131, 15),
}


def decode_camf(raw: bytes) -> CamfFields:
    values = [read_bits(raw, *CAMF_TRACE[f"A{index}"])
              for index in range(1, 19)]
    return CamfFields(*values)

