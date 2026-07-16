"""Versioned CAMF/DCX lookup adapters."""

from .._vendor.azarashi.qzss_dcr_lib.definition.qzss_dcx_camf_a4_hazard_category_and_type import (
    qzss_dcx_camf_a4_hazard_category,
    qzss_dcx_camf_a4_hazard_definition,
    qzss_dcx_camf_a4_hazard_type,
)
from ..models import AlertCode


def hazard(code: int) -> AlertCode:
    if 1 <= code <= 113:
        token = str(qzss_dcx_camf_a4_hazard_type[code]).upper().replace(" ", "_").replace("/", "_")
        return AlertCode(code, token, True, False, None,
                         str(qzss_dcx_camf_a4_hazard_type[code]))
    return AlertCode(code, None, False, code == 0 or code >= 114)


def hazard_detail(code: int) -> dict:
    if not 1 <= code <= 113:
        return {"code": code, "known": False}
    return {
        "code": code,
        "known": True,
        "category": qzss_dcx_camf_a4_hazard_category[code],
        "type": qzss_dcx_camf_a4_hazard_type[code],
        "definition": qzss_dcx_camf_a4_hazard_definition[code],
        "spec_version": "CAMF-1.2",
    }


def all_hazards() -> tuple[dict, ...]:
    return tuple(hazard_detail(code) for code in range(1, 114))

