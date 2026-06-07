"""Checksum computation — SHA256 over all input YAML content."""

import hashlib
import os


def compute(registry: "RegistryAST", drivers_dir: str) -> int:
    """Compute deterministic SHA256 checksum over all input YAML files.

    Files are read in sorted-path order for determinism.
    Returns the first 32 bits of the SHA256 digest as an integer.
    """
    sha = hashlib.sha256()

    # collect all YAML paths in sorted order
    yaml_paths = [registry.file_path]
    for drv in registry.drivers:
        if drv.status == "active" and drv.file_path:
            yaml_paths.append(drv.file_path)

    yaml_paths.sort()

    for path in yaml_paths:
        with open(path, "rb") as f:
            sha.update(f.read())

    digest = sha.digest()
    # first 4 bytes as uint32 (big-endian)
    return int.from_bytes(digest[:4], "big")
