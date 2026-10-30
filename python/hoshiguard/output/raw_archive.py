"""破損から復帰でき、チェックサムを持つ HoshiGuard raw Bridge アーカイブ。"""

from __future__ import annotations

from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path
import os
import struct
import time

from ..bridge_protocol import (
    BridgeError, BridgeFrame, CobsError, CrcError, VersionError,
    crc32_iso_hdlc, decode_wire,
)

FILE_HEADER = struct.Struct("<8sHHI")
FILE_MAGIC = b"HGRAW1\0\0"
RECORD_HEADER = struct.Struct("<4sIQI")
RECORD_MAGIC = b"HGRF"
MAX_RECORD = 4096


@dataclass(frozen=True)
class ArchiveRecord:
    pc_receive_ns: int
    wire: bytes
    frame: BridgeFrame
    offset: int


@dataclass
class ArchiveReadStats:
    records_seen: int = 0
    valid_records: int = 0
    recovered_records: int = 0
    invalid_lengths: int = 0
    truncated_records: int = 0
    archive_crc_errors: int = 0
    wire_length_errors: int = 0
    cobs_errors: int = 0
    bridge_crc_errors: int = 0
    bridge_version_errors: int = 0
    bridge_structure_errors: int = 0
    skipped_bytes: int = 0


class RawArchiveWriter:
    def __init__(self, path: str, flush_every: int = 16,
                 rotate_bytes: int = 0, rotate_seconds: float = 0,
                 fsync_interval: float = 0, write_sha256: bool = True):
        self.base = Path(path)
        self.flush_every = max(1, flush_every)
        self.rotate_bytes = rotate_bytes
        self.rotate_seconds = max(0.0, rotate_seconds)
        self.fsync_interval = max(0.0, fsync_interval)
        self.write_sha256 = write_sha256
        self.count = 0
        self.index = 0
        self.stream = None
        self.path = self.base
        self.opened_monotonic = time.monotonic()
        self.last_fsync = self.opened_monotonic
        self._digest = sha256()
        self._open()

    def _open(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists() and self.path.stat().st_size:
            with self.path.open("rb") as source:
                header = source.read(FILE_HEADER.size)
            if header != FILE_HEADER.pack(FILE_MAGIC, 1, 0, 0):
                raise ValueError("refusing to append to an unsupported raw archive")
        self.stream = self.path.open("ab+")
        self._digest = sha256()
        if self.stream.tell() == 0:
            header = FILE_HEADER.pack(FILE_MAGIC, 1, 0, 0)
            self.stream.write(header)
            self._digest.update(header)
        else:
            # 既存ファイルはプロセス再開時に一度だけ走査する。分割ファイルは新しい一意な名前とし、分割のたびに巨大なアーカイブを再読しない。
            with self.path.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    self._digest.update(chunk)
        self.opened_monotonic = time.monotonic()
        self.last_fsync = self.opened_monotonic

    def _write_sha_sidecar(self) -> None:
        if not self.write_sha256:
            return
        self.path.with_suffix(self.path.suffix + ".sha256").write_text(
            f"{self._digest.hexdigest()}  {self.path.name}\n", encoding="ascii")

    def _rotate(self) -> None:
        assert self.stream is not None
        self.stream.flush()
        os.fsync(self.stream.fileno())
        self.stream.close()
        self._write_sha_sidecar()
        self.index += 1
        stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())
        self.path = self.base.with_name(
            f"{self.base.stem}.{stamp}.{time.time_ns() % 1_000_000_000:09d}."
            f"{self.index:04d}{self.base.suffix}")
        self._open()

    def write(self, wire: bytes, pc_receive_ns: int | None = None) -> None:
        assert self.stream is not None
        if not wire.endswith(b"\0"):
            raise ValueError("archive requires a complete delimited Bridge frame")
        decode_wire(wire)  # 構造が正しい Bridge レコードのみ保存する
        pc_receive_ns = time.time_ns() if pc_receive_ns is None else pc_receive_ns
        body = struct.pack("<QI", pc_receive_ns, len(wire)) + wire
        record = RECORD_MAGIC + struct.pack("<I", len(body) + 4) + body
        record += struct.pack("<I", crc32_iso_hdlc(body))
        rotate_size = (self.rotate_bytes and
                       self.stream.tell() + len(record) > self.rotate_bytes)
        rotate_time = (self.rotate_seconds and
                       time.monotonic() - self.opened_monotonic >= self.rotate_seconds)
        if rotate_size or rotate_time:
            self._rotate()
        self.stream.write(record)
        self._digest.update(record)
        self.count += 1
        if self.count % self.flush_every == 0:
            self.stream.flush()
        now = time.monotonic()
        if self.fsync_interval and now - self.last_fsync >= self.fsync_interval:
            self.stream.flush()
            os.fsync(self.stream.fileno())
            self.last_fsync = now

    def close(self) -> None:
        if self.stream is not None:
            self.stream.flush()
            os.fsync(self.stream.fileno())
            self.stream.close()
            self._write_sha_sidecar()
            self.stream = None

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()


class RawArchiveReader:
    def __init__(self, path: str):
        self.path = Path(path)
        self.corrupt_records = 0
        self.stats = ArchiveReadStats()
        self.archive_major = 0
        self.archive_minor = 0

    @staticmethod
    def _find_magic(stream, start: int) -> int | None:
        """上限付き chunk で前方走査し、アーカイブ全体を読み込まない。"""
        stream.seek(start)
        overlap = b""
        absolute = start
        while True:
            chunk = stream.read(8192)
            if not chunk:
                return None
            data = overlap + chunk
            found = data.find(RECORD_MAGIC)
            if found >= 0:
                return absolute - len(overlap) + found
            overlap = data[-(len(RECORD_MAGIC) - 1):]
            absolute += len(chunk)

    def __iter__(self):
        self.stats = ArchiveReadStats()
        self.corrupt_records = 0
        size = self.path.stat().st_size
        with self.path.open("rb") as stream:
            header = stream.read(FILE_HEADER.size)
            if len(header) < FILE_HEADER.size:
                raise ValueError("truncated HoshiGuard raw archive header")
            magic, major, minor, _flags = FILE_HEADER.unpack(header)
            if magic != FILE_MAGIC or major != 1:
                raise ValueError("unsupported HoshiGuard raw archive")
            self.archive_major, self.archive_minor = major, minor
            offset = FILE_HEADER.size
            recovered = False
            while offset + 8 <= size:
                found = self._find_magic(stream, offset)
                if found is None:
                    self.stats.skipped_bytes += max(0, size - offset)
                    break
                if found != offset:
                    self.stats.skipped_bytes += found - offset
                    recovered = True
                offset = found
                stream.seek(offset + 4)
                raw_length = stream.read(4)
                if len(raw_length) != 4:
                    self.stats.truncated_records += 1
                    break
                length, = struct.unpack("<I", raw_length)
                self.stats.records_seen += 1
                if length < 16 or length > MAX_RECORD:
                    self.stats.invalid_lengths += 1
                    self.corrupt_records += 1
                    offset += 1
                    recovered = True
                    continue
                record = stream.read(length)
                if len(record) != length:
                    self.stats.truncated_records += 1
                    self.corrupt_records += 1
                    offset += 1
                    recovered = True
                    continue
                body, raw_crc = record[:-4], record[-4:]
                received, = struct.unpack("<I", raw_crc)
                if crc32_iso_hdlc(body) != received:
                    self.stats.archive_crc_errors += 1
                    self.corrupt_records += 1
                    offset += 1
                    recovered = True
                    continue
                pc_ns, wire_length = struct.unpack_from("<QI", body)
                wire = body[12:]
                if wire_length != len(wire):
                    self.stats.wire_length_errors += 1
                    self.corrupt_records += 1
                    offset += 1
                    recovered = True
                    continue
                try:
                    frame = decode_wire(wire)
                except CobsError:
                    self.stats.cobs_errors += 1
                except CrcError:
                    self.stats.bridge_crc_errors += 1
                except VersionError:
                    self.stats.bridge_version_errors += 1
                except BridgeError:
                    self.stats.bridge_structure_errors += 1
                else:
                    self.stats.valid_records += 1
                    if recovered:
                        self.stats.recovered_records += 1
                        recovered = False
                    yield ArchiveRecord(pc_ns, wire, frame, offset)
                    offset += 8 + length
                    continue
                self.corrupt_records += 1
                offset += 1
                recovered = True


def archive_sha256(path: str) -> str:
    digest = sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()
