"""Guidance lookup is decoded by the CAMF library; raw A11 is always retained."""


def guidance_code(fields: dict, raw_code: int) -> dict:
    return {
        "code": raw_code,
        "international": fields.get("a11_international_library"),
        "japanese": fields.get("a11_japanese_library"),
        "japanese_official": fields.get("a11_japanese_library_ja"),
    }

