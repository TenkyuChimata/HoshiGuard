"""Audited specification inventory access.

The inventory is maintained in ``docs/spec/inventory.json`` by the repository
checks.  Parser modules cite its stable spec identifiers instead of embedding
an untraceable document revision in each decoder.
"""

from pathlib import Path


def repository_inventory() -> Path:
    """Return the source-tree inventory path when running from a checkout."""
    return Path(__file__).resolve().parents[3] / "docs/spec/inventory.json"


__all__ = ["repository_inventory"]
