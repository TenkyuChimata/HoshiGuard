"""Optional-pyserial reconnecting Bridge receiver."""

from __future__ import annotations

from dataclasses import dataclass
import time
from typing import Callable

from .bridge_protocol import (
    FrameType, SequenceDisposition, SequenceTracker, StreamDecoder,
)


@dataclass
class ReceiverStats:
    reconnects: int = 0
    disconnects: int = 0
    frames: int = 0
    sequence_gaps: int = 0
    duplicate_sequences: int = 0
    out_of_order_sequences: int = 0
    epochs: int = 0
    waiting_for_hello: bool = True


class SerialReceiver:
    def __init__(self, port: str, baud: int = 115200,
                 reconnect: bool = True, reconnect_delay: float = 1.0,
                 status_callback: Callable[[str], None] | None = None):
        self.port = port
        self.baud = baud
        self.reconnect = reconnect
        self.reconnect_delay = reconnect_delay
        self.decoder = StreamDecoder()
        self.sequence = SequenceTracker()
        self.stats = ReceiverStats()
        self.status_callback = status_callback

    def _status(self, message: str) -> None:
        if self.status_callback is not None:
            self.status_callback(message)

    def frames(self, stop=lambda: False):
        try:
            import serial
        except ImportError as exc:
            raise RuntimeError("live reception requires pyserial") from exc
        while not stop():
            connection = None
            try:
                connection = serial.Serial(self.port, self.baud, timeout=0.25)
                self.stats.reconnects += 1
                self.stats.waiting_for_hello = True
                self._status(f"bridge_serial_connected port={self.port} "
                             f"baud={self.baud} waiting_for_hello=yes")
                while not stop():
                    chunk = connection.read(256)
                    for frame in self.decoder.feed(chunk):
                        hello = frame.frame_type == FrameType.HELLO
                        result = self.sequence.observe(frame.sequence, hello=hello)
                        if result.missing:
                            self.stats.sequence_gaps += result.missing
                            self._status(f"bridge_sequence_gap missing={result.missing} "
                                         f"sequence={frame.sequence} epoch={result.epoch}")
                        if result.disposition == SequenceDisposition.DUPLICATE:
                            self.stats.duplicate_sequences += 1
                        elif result.disposition == SequenceDisposition.OUT_OF_ORDER:
                            self.stats.out_of_order_sequences += 1
                        elif result.disposition == SequenceDisposition.NEW_EPOCH:
                            self.stats.epochs = result.epoch
                            self._status(f"bridge_new_epoch epoch={result.epoch} "
                                         f"sequence={frame.sequence}")
                        if self.stats.waiting_for_hello and not hello:
                            continue
                        if hello:
                            self.stats.waiting_for_hello = False
                            self._status(f"bridge_hello sequence={frame.sequence} "
                                         f"epoch={result.epoch}")
                        self.stats.frames += 1
                        yield frame, time.time_ns()
            except (OSError, serial.SerialException):
                self.stats.disconnects += 1
                self.decoder.discard_partial()
                self._status(f"bridge_serial_disconnected port={self.port} "
                             f"reconnect={'yes' if self.reconnect else 'no'}")
                if not self.reconnect:
                    raise
                time.sleep(self.reconnect_delay)
            finally:
                if connection is not None:
                    connection.close()
