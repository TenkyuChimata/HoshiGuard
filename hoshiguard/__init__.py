"""Repository-root shim for ``python3 -m hoshiguard``."""

from pathlib import Path

_implementation = Path(__file__).resolve().parent.parent / "python" / "hoshiguard"
if str(_implementation) not in __path__:
    __path__.append(str(_implementation))

__version__ = "0.2.7"
