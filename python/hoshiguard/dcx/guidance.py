"""指示文は CAMF ライブラリで解読し、A11 原値を常に保持する。"""


def guidance_code(fields: dict, raw_code: int) -> dict:
    return {
        "code": raw_code,
        "international": fields.get("a11_international_library"),
        "japanese": fields.get("a11_japanese_library"),
        "japanese_official": fields.get("a11_japanese_library_ja"),
    }

