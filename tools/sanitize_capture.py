#!/usr/bin/env python3
"""バイナリノイズを除き、任意でプライバシー処理した捕捉データのコピーを作る。"""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import tempfile
from typing import Optional

import capture_io
import replay_ubx


def _is_nav_pvt_raw(line: str) -> bool:
    records, _ = replay_ubx.parse_log((line,))
    return bool(records and records[0].packet[2:4] == b"\x01\x07")


def sanitize(capture: capture_io.CaptureInput, strip_nav_pvt: bool) -> tuple[bytes, int, int]:
    kept: list[str] = []
    removed_nav = 0
    removed_coordinates = 0
    for line in capture.lines:
        if strip_nav_pvt and _is_nav_pvt_raw(line):
            removed_nav += 1
            continue
        if strip_nav_pvt and (line.startswith("NAV_COORD ") or
                              " lon_1e7=" in line or " lat_1e7=" in line):
            removed_coordinates += 1
            continue
        kept.append(line)
    output = ("\n".join(kept) + ("\n" if kept else "")).encode("ascii")
    return output, removed_nav, removed_coordinates


def atomic_write(path: pathlib.Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".",
                                             dir=str(path.parent))
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--strip-nav-pvt", action="store_true")
    args = parser.parse_args(argv)
    if args.capture.resolve() == args.output.resolve():
        parser.error("output must not replace the source capture")
    try:
        source = capture_io.read_capture(args.capture)
        clean, removed_nav, removed_coordinates = sanitize(
            source, args.strip_nav_pvt)
        atomic_write(args.output, clean)
    except OSError as exc:
        print(f"error: {exc}")
        return 2
    print(f"source_sha256={source.sha256}")
    print(f"output_sha256={hashlib.sha256(clean).hexdigest()}")
    print(f"leading_garbage_removed={source.leading_garbage_bytes}")
    print(f"records_retained={len(clean.splitlines())}")
    print(f"nav_pvt_raw_removed={removed_nav}")
    print(f"coordinate_records_removed={removed_coordinates}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
