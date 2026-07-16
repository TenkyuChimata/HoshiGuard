"""IS-QZSS-DCX-004 Extended Message fields.

EX11 is the 68-bit outside-Japan extension. DCX-004 intentionally leaves its
semantic layout to the relevant country/region; HoshiGuard retains those bits
and marks them blocked instead of inventing fields.
"""

from __future__ import annotations

from ..l1s import read_bits
from .models import CamfFields, ExtendedFields


def decode_extended(raw: bytes, camf: CamfFields) -> ExtendedFields:
    version = read_bits(raw, 214, 6)
    extension_is_zero = (read_bits(raw, 146, 32) == 0 and
                         read_bits(raw, 178, 32) == 0 and
                         read_bits(raw, 210, 4) == 0)
    # DCX-004 Null keeps A2=111 (Japan); all other A/EX payload fields are zero.
    if all(value == 0 for index, value in enumerate(camf.__dict__.values())
           if index != 1) and camf.a2_country_region == 111 and extension_is_zero:
        return ExtendedFields("null", (), version)
    if camf.a2_country_region != 111:
        fields = (("EX11_raw_high", read_bits(raw, 146, 32)),
                  ("EX11_raw_mid", read_bits(raw, 178, 32)),
                  ("EX11_raw_low", read_bits(raw, 210, 4)),)
        return ExtendedFields("outside_japan_ex11", fields, version,
                              ("EX11 country-specific semantic definition",))
    if camf.a3_provider in (2, 3):
        fields = (("EX8_target_area_list_type", read_bits(raw, 146, 1)),
                  ("EX9_target_area_code_list", read_bits(raw, 147, 64)),
                  ("EX10_reserved", read_bits(raw, 211, 3)))
        return ExtendedFields("j_alert_ex8_ex9", fields, version)
    if camf.a3_provider == 1:
        if any((camf.a12_latitude, camf.a13_longitude,
                camf.a14_semi_major, camf.a15_semi_minor,
                camf.a16_azimuth)):
            return ExtendedFields("l_alert_main_ellipse", (), version)
        return ExtendedFields(
            "l_alert_ex1",
            (("EX1_target_area_code", read_bits(raw, 146, 16)),),
            version)
    if camf.a3_provider == 4:
        fields = (("EX1_target_area_code", read_bits(raw, 146, 16)),
                  ("EX2_evacuate_direction_type", read_bits(raw, 162, 1)),
                  ("EX3_additional_latitude", read_bits(raw, 163, 17)),
                  ("EX4_additional_longitude", read_bits(raw, 180, 17)),
                  ("EX5_additional_semi_major", read_bits(raw, 197, 5)),
                  ("EX6_additional_semi_minor", read_bits(raw, 202, 5)),
                  ("EX7_additional_azimuth", read_bits(raw, 207, 7)))
        return ExtendedFields("local_government_ex1_ex7", fields, version)
    fields = (("EX_unknown_raw_high", read_bits(raw, 146, 32)),
              ("EX_unknown_raw_mid", read_bits(raw, 178, 32)),
              ("EX_unknown_raw_low", read_bits(raw, 210, 4)))
    return ExtendedFields("japan_unknown_provider", fields, version,
                          ("unknown Japan provider extension layout",))
